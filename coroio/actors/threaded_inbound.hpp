#pragma once

#include "actorsystem.hpp"

#include <coroio/accept_errors.hpp>
#include <coroio/mpsc_queue.hpp>
#include <coroio/notifier.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace NNet {
namespace NActors {

#ifndef _WIN32

// TODO: a batch is allocated on every read and freed on the actor thread. Recycle batches
// through a return queue with a cap on spares, as TThreadedNode does with its segments.
/// Envelopes of one read of an inbound connection, on their way from its I/O thread to the actors.
struct TEnvelopeBatch : TMpscQueueNode<TEnvelopeBatch> {
    std::vector<TEnvelope> Envelopes;
    /// Payload and header bytes of the envelopes
    size_t Bytes = 0;
};

/**
 * @brief Delivery of `TThreadedInbound` to an actor system on another thread.
 *
 * - **I/O side** — `Deliver` pushes a batch into a `TMpscQueue` and wakes the actor side
 *   through a notifier of the actor system's poller if the queue was empty. Any number
 *   of I/O threads may deliver.
 * - **Actor side** — a coroutine on that poller pops the batches and calls
 *   `TActorSystem::Send` for every envelope, in the order of `Deliver` calls of each
 *   thread. After every 2 MiB it lets the loop run its other coroutines and the actors,
 *   as the connections of `TActorSystem::Serve(TSocket)` do.
 *
 * `Full()` is true while the bytes handed over and not passed to `Send` yet reach `limit`.
 *
 * @note Construct and destroy it on the thread of the actor system, before its loop
 *       runs and after the I/O threads that deliver to it have stopped. The destructor
 *       deletes the batches that were not delivered. The actor system outlives it.
 * @note Not available on Windows: it needs `TNotifier`.
 */
class TQueuedDelivery {
public:
    static constexpr size_t DefaultLimit = 16 * 1024 * 1024;

    explicit TQueuedDelivery(TActorSystem& system, size_t limit = DefaultLimit)
        : System_(system)
        , Limit_(limit)
        , Notifier_(system.CreateNotifier())
        , Consumer_(Consume())
    { }

    /// Destroys the coroutine first: it waits on the notifier and pops the queue
    ~TQueuedDelivery() {
        Consumer_ = TFuture<void>();
    }

    TQueuedDelivery(const TQueuedDelivery&) = delete;
    TQueuedDelivery& operator=(const TQueuedDelivery&) = delete;

    /// I/O side: true while the connections must not read
    bool Full() const {
        return Queued_.load(std::memory_order_relaxed) >= Limit_;
    }

    /// I/O side
    void Deliver(std::unique_ptr<TEnvelopeBatch> batch) {
        // Relaxed: the count orders nothing, the batch itself goes through Queue_
        Queued_.fetch_add(batch->Bytes, std::memory_order_relaxed);
        if (Queue_.Push(std::move(batch))) {
            Notifier_->Notify();
        }
    }

    /// Bytes handed over and not passed to `Send` yet. Any thread.
    size_t QueuedBytes() const {
        return Queued_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t MaxBytesBeforeYield = 2 * 1024 * 1024;

    // Actor side. A failed wait is logged and tried again after a second, as in TThreadedNode.
    TFuture<void> WaitForWork() {
        try {
            co_await Notifier_->Wait();
            co_return;
        } catch (const std::exception& ex) {
            std::cerr << "Error waiting for inbound envelopes: " << ex.what() << "\n";
        }
        co_await System_.Sleep(std::chrono::milliseconds(1000));
    }

    // Actor side
    TFuture<void> Consume() {
        size_t delivered = 0;
        while (true) {
            auto batch = Queue_.Pop();
            if (!batch) {
                co_await WaitForWork();
                continue;
            }
            for (auto& envelope : batch->Envelopes) {
                System_.Send(envelope.Sender, envelope.Recipient, envelope.MessageId, std::move(envelope.Blob));
            }
            Queued_.fetch_sub(batch->Bytes, std::memory_order_relaxed);
            delivered += batch->Bytes;
            // Not Yield(): resumed from a timer, it is resumed again in the same pass (issue 244)
            if (delivered >= MaxBytesBeforeYield) {
                delivered = 0;
                co_await System_.Sleep(std::chrono::nanoseconds(1));
            }
        }
    }

    TActorSystem& System_;
    const size_t Limit_;
    std::unique_ptr<INotifier> Notifier_;
    TMpscQueue<TEnvelopeBatch> Queue_;
    std::atomic<size_t> Queued_ = 0;
    TFuture<void> Consumer_;
};

/**
 * @brief Accepts and reads the inbound connections of an actor system on an I/O thread.
 *
 * The accept loop and one coroutine per connection run on `poller`, which belongs to
 * another thread (see `TLoopThread`). A connection reads its socket into its own
 * `TEnvelopeReader`, as a connection of `TActorSystem::Serve(TSocket)` does, and hands
 * the envelopes of every read to `delivery` in one `TEnvelopeBatch`. Envelopes for
 * another node are dropped and logged, as there.
 *
 * `TDelivery` is the way to the actors; `TQueuedDelivery` is the one for an actor
 * system on another thread. Both calls are made on the I/O thread:
 *
 * - `bool Full() const` — while it is true, the connections do not read: the bytes stay
 *   in the sockets, and TCP slows the senders down. A connection asks before each read,
 *   so each one hands over at most one read beyond the limit of the delivery.
 * - `void Deliver(std::unique_ptr<TEnvelopeBatch> batch)`.
 *
 * A finished connection closes its socket; the inbound forgets it at the next accept.
 *
 * @code
 * TLoop<TDefaultPoller> loop;               // the loop of the actor system
 * TActorSystem sys(&loop.Poller(), 1);
 * TLoopThread<TDefaultPoller> io;
 * sys.AddNode(2, ...);                       // a TNode on loop or a TThreadedNode on io
 * TQueuedDelivery delivery(sys);
 * TAddress address{"::", 9001};
 * TDefaultPoller::TSocket listener(io.Poller(), address.Domain());
 * listener.Bind(address);
 * listener.Listen();
 * TThreadedInbound<TDefaultPoller, TQueuedDelivery, TZeroCopyEnvelopeReaderV2> inbound(
 *     io.Poller(), std::move(listener), 1, delivery);
 * sys.ServeOutbound();
 * io.Start();
 * ...
 * io.Stop();
 * io.Join();
 * // inbound and then delivery are destroyed here, before io and loop
 * @endcode
 *
 * @tparam TPoller         Poller type of the I/O thread; determines the socket type.
 * @tparam TDelivery       Receives the batches, see above.
 * @tparam TEnvelopeReader `TZeroCopyEnvelopeReader`, which copies every payload into an
 *                         allocation of its own, or `TZeroCopyEnvelopeReaderV2`, whose
 *                         payloads point into its chunks and may be released on any thread.
 *
 * @note Construct it before the loop of `poller` starts and destroy it after that loop
 *       has stopped: both touch the poller. `listener` is a listening socket on `poller`.
 * @note The logger is called on the I/O thread.
 * @note Not available on Windows: I/O threads need `TNotifier`.
 */
template<typename TPoller, typename TDelivery, typename TEnvelopeReader = TZeroCopyEnvelopeReader>
class TThreadedInbound {
public:
    using TSocket = typename TPoller::TSocket;

    /**
     * @param poller   Poller of the I/O thread.
     * @param listener Bound, listening socket on `poller`.
     * @param nodeId   Node of the actor system: envelopes for other nodes are dropped.
     * @param delivery Receives the envelopes; outlives the inbound.
     * @param logger   Optional; called on the I/O thread.
     */
    TThreadedInbound(TPoller& poller, TSocket listener, TNodeId nodeId, TDelivery& delivery, std::function<void(const std::string&)> logger = {})
        : Poller_(poller)
        , Listener_(std::move(listener))
        , NodeId_(nodeId)
        , Delivery_(delivery)
        , Logger_(std::move(logger))
        , Acceptor_(Accept())
    { }

    /// Destroys the coroutines first: they wait on the sockets
    ~TThreadedInbound() {
        Acceptor_ = TFuture<void>();
        Connections_.clear();
    }

    TThreadedInbound(const TThreadedInbound&) = delete;
    TThreadedInbound& operator=(const TThreadedInbound&) = delete;

    /// Connections accepted and not forgotten yet. On the I/O thread, or while its loop does not run.
    size_t Connections() const {
        return Connections_.size();
    }

private:
    struct TConnection {
        explicit TConnection(TSocket socket)
            : Socket(std::move(socket))
        { }

        TSocket Socket;
        // Declared after Socket, so it is destroyed first
        TFuture<void> Reader;
    };

    static constexpr size_t ReadSize = 512 * 1024;
    static constexpr size_t InflightBytes = 16 * 1024 * 1024;
    static constexpr size_t MaxBytesBeforeYield = 2 * 1024 * 1024;

    TFuture<void> Accept() {
        while (true) {
            auto client = co_await Listener_.Accept();
            if (client) {
                std::erase_if(Connections_, [](const auto& connection) {
                    return connection->Reader.done();
                });
                client->SetNoDelay();
                auto& connection = *Connections_.emplace_back(std::make_unique<TConnection>(std::move(*client)));
                connection.Reader = Read(connection.Socket);
            } else if (!IsTransientAcceptError(client.error())) {
                if (Logger_) {
                    Logger_("Accept failed: " + std::system_category().message(client.error()));
                }
                co_await Poller_.Sleep(std::chrono::milliseconds(100));
            }
        }
    }

    TFuture<void> Read(TSocket& socket) {
        TEnvelopeReader envelopeReader(InflightBytes, /*lowWatermark = */ 1024);
        size_t bytesRead = 0;
        try {
            while (true) {
                while (Delivery_.Full()) {
                    co_await Poller_.Sleep(std::chrono::milliseconds(1));
                }
                auto buffer = envelopeReader.Acquire(ReadSize);
                auto size = co_await socket.ReadSome(buffer.data(), buffer.size());
                if (size < 0) {
                    continue;
                }
                if (size == 0) {
                    throw std::runtime_error("Socket closed");
                }
                envelopeReader.Commit(size);

                std::unique_ptr<TEnvelopeBatch> batch;
                while (auto envelope = envelopeReader.Pop()) {
                    if (envelope->Recipient.NodeId() != NodeId_) [[unlikely]] {
                        if (Logger_) {
                            Logger_("Received message for different node: " + envelope->Recipient.ToString());
                        }
                        continue;
                    }
                    if (!batch) {
                        batch = std::make_unique<TEnvelopeBatch>();
                    }
                    batch->Bytes += envelope->Blob.Size + sizeof(THeader);
                    batch->Envelopes.push_back(std::move(*envelope));
                }
                if (batch) {
                    Delivery_.Deliver(std::move(batch));
                }

                // A read that does not block does not suspend: without this a busy peer
                // would keep the loop from the other connections. Not Yield(), see TThreadedNode.
                bytesRead += size;
                if (bytesRead >= MaxBytesBeforeYield) {
                    bytesRead = 0;
                    co_await Poller_.Sleep(std::chrono::nanoseconds(1));
                }
            }
        } catch (const std::exception& e) {
            if (Logger_) {
                Logger_(std::string("Error in InboundConnection: ") + e.what());
            }
        }
        socket.Close();
    }

    TPoller& Poller_;
    TSocket Listener_;
    TNodeId NodeId_;
    TDelivery& Delivery_;
    std::function<void(const std::string&)> Logger_;
    std::vector<std::unique_ptr<TConnection>> Connections_;
    TFuture<void> Acceptor_;
};

#endif // _WIN32

} // namespace NActors
} // namespace NNet
