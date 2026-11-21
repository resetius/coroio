#pragma once

#include "actor.hpp"
#include "envelope_reader.hpp"
#include "messages_factory.hpp"

#include <coroio/address.hpp>
#include <coroio/dns/resolver.hpp>

#include <span>

namespace NNet {
namespace NActors {

/**
 * @brief Abstract write-buffer interface used by the actor transport layer.
 *
 * Follows an acquire/commit pattern: call `Acquire` to reserve contiguous
 * space, write into it, then call `Commit` with the number of bytes actually
 * written. This allows zero-copy serialization directly into network buffers.
 */
class IOutputStream {
public:
    virtual ~IOutputStream() = default;
    /// Reserves `size` contiguous bytes after the committed ones and returns a span over them.
    /// A reservation that is not committed (its writer threw) is reused by the next `Acquire`.
    virtual std::span<char> Acquire(size_t size) = 0;
    /// Marks `size` previously acquired bytes as ready to send.
    virtual void Commit(size_t size) = 0;
    /// Stream offset of the next `Acquire`, or a value congruent to it mod `MaxPayloadAlignment`:
    /// payloads with `PayloadAlignment` are padded relative to it. The default suits a stream
    /// that keeps each frame in its own buffer.
    virtual uint64_t Offset() const {
        return 0;
    }
};

/**
 * @brief Interface for a remote node connection in the actor transport layer.
 *
 * Extends `IOutputStream` with the ability to enqueue serialized messages,
 * initiate/maintain a TCP connection, and flush the outbound buffer.
 * `TActorSystem` holds one `INode` per remote host:port pair.
 */
class INode : public IOutputStream {
public:
    virtual ~INode() = default;
    /// Serializes `envelope` and appends it to the outbound buffer.
    virtual void Send(TEnvelope&& envelope) = 0;
    /// Initiates an async TCP connection if not already connected or connecting.
    virtual void StartConnect() = 0;
    /// Calls `StartConnect()` then asynchronously writes all buffered bytes to the socket.
    virtual void Drain() = 0;
    /// Returns the remote host:port this node connects to.
    virtual THostPort GetHostPort() const = 0;
};

/**
 * @brief Concrete `INode` implementation for a single remote actor-system endpoint.
 *
 * Maintains a persistent TCP connection to `hostPort`, reconnecting with a 1 s
 * back-off on failure. Outbound messages are accumulated in a vector buffer and
 * flushed by `Drain()` once the connection is established.
 *
 * @tparam TPoller Poller type (e.g. `TDefaultPoller`); determines the socket type.
 */
template<typename TPoller>
class TNode final : public INode {
public:
    using TSocket = typename TPoller::TSocket;
    /**
     * @brief Constructs a TNode but does not connect immediately.
     *
     * Call `StartConnect()` or `Drain()` to initiate the connection.
     *
     * @param poller        Poller used for async I/O and sleep.
     * @param factory       Factory for serializing messages to their Far (wire) form.
     * @param resolver      DNS resolver for translating `hostPort.Host` to an IP.
     * @param socketFactory Callable that creates a connected socket given a resolved `TAddress`.
     * @param hostPort      Remote host and port to connect to.
     */
    TNode(TPoller& poller, TMessagesFactory& factory, TResolver& resolver, const std::function<TSocket(const TAddress&)>& socketFactory, const THostPort& hostPort)
        : Poller(poller)
        , Factory(factory)
        , Resolver(resolver)
        , SocketFactory(socketFactory)
        , HostPort(hostPort)
    { }

    /// Destroys the coroutines first: a pending write reads `SendBuffer` and a pending
    /// connect uses `Socket`, and on io_uring the kernel keeps reading them until the
    /// coroutine's destructor has cancelled the operation. This must not depend on the
    /// member declaration order.
    ~TNode() {
        Drainer = TFuture<void>();
        Connector = TFuture<void>();
    }

    // TODO: remove me, envelope needed only for local actors
    void Send(TEnvelope&& envelope) override {
        // Empty blobs skip the factory and its compile-time check of the id
        if (envelope.MessageId > MaxMessageId) [[unlikely]] {
            throw std::invalid_argument("message id " + std::to_string(envelope.MessageId)
                + " overlaps the padding bits of THeader::MessageId");
        }
        auto blob = std::move(envelope.Blob);
        uint32_t padding = 0;
        // TODO: a blob already in Far form (a received non-POD message Forward-ed to another
        // node) is serialized again as if it held a T object; pass such blobs through as bytes.
        if (blob.Size > 0) {
            blob = Factory.SerializeFar(envelope.MessageId, std::move(blob));
            const size_t alignment = Factory.PayloadAlignment(envelope.MessageId);
            if (alignment > 1) {
                padding = PayloadPadding(Offset(), alignment);
            }
        }
        THeader data{
            .Sender = envelope.Sender,
            .Recipient = envelope.Recipient,
            .MessageId = envelope.MessageId | (padding << HeaderPaddingShift),
            .Size = blob.Size
        };
        const size_t size = sizeof(data) + padding + blob.Size;
        auto buf = Acquire(size);
        std::memcpy(buf.data(), &data, sizeof(data));
        std::memset(buf.data() + sizeof(data), 0, padding);
        if (blob.Size > 0) {
            std::memcpy(buf.data() + sizeof(data) + padding, blob.Data.get(), blob.Size);
        }
        Commit(size);
    }

    std::span<char> Acquire(size_t size) override {
        if (CommittedBytes + size > OutputBuffer.size()) {
            OutputBuffer.resize(CommittedBytes + size);
        }
        return std::span<char>(OutputBuffer.data() + CommittedBytes, size);
    }

    void Commit(size_t size) override {
        CommittedBytes += size;
    }

    uint64_t Offset() const override {
        return SentBytes + CommittedBytes;
    }

    void StartConnect() override {
        if (!Connected) {
            Connect();
            return;
        }
    }

    void Drain() override {
        StartConnect();
        StartDrain();
    }

    THostPort GetHostPort() const override {
        return HostPort;
    }

private:
    void StartDrain() {
        if (Connected && (!Drainer.raw() || Drainer.done())) {
            Drainer = DoDrain();
        }
    }

    TFuture<void> DoDrain() {
        try {
            while (CommittedBytes > 0) {
                SendBuffer.swap(OutputBuffer);
                auto readSize = CommittedBytes;
                CommittedBytes = 0;
                SentBytes += readSize;
                co_await TByteWriter(Socket).Write(SendBuffer.data(), readSize);
            }
            co_return;
        } catch (const std::exception& ex) {
            std::cerr << "Error during draining: " << ex.what() << "\n";
            Connect();
            co_return;
        }
    }

    void Connect() {
        if (Connector.raw() && !Connector.done()) {
            return;
        }

        Connector = DoConnect();
    }

    TFuture<void> DoConnect() {
        Connected = false;
        std::cout << "Connecting to " << HostPort.ToString() << "\n";
        while (!Connected) {
            try {
                //auto deadline = NNet::TClock::now() + std::chrono::milliseconds(5000);
                TAddress addr = co_await HostPort.Resolve(Resolver);
                Socket = SocketFactory(addr);
                // Small messages (acks) must not wait for Nagle and delayed ACK.
                Socket.SetNoDelay();
                co_await Socket.Connect(addr /*, deadline*/);
                // The queued frames are padded for the old stream offset: the filler gives them
                // the same offsets mod MaxPayloadAlignment in the new stream, and they stay as is.
                char filler[MaxFillerSize];
                if (const size_t size = WriteFiller(filler, SentBytes)) {
                    co_await TByteWriter(Socket).Write(filler, size);
                }
                Connected = true;
                std::cout << "Connected to " << HostPort.ToString() << "\n";
            } catch (const std::exception& ex) {
                std::cerr << "Error connecting to " << HostPort.ToString() << ": " << ex.what() << "\n";
                Connected = false;
            }
            if (!Connected) {
                co_await Poller.Sleep(std::chrono::milliseconds(1000));
            }
        }
        // Nothing calls Drain() again for bytes queued while connecting (or left after a send error).
        if (CommittedBytes > 0) {
            StartDrain();
        }
        co_return;
    }

    bool Connected = false;
    TFuture<void> Drainer;
    TFuture<void> Connector;
    TSocket Socket;

    TPoller& Poller;
    TMessagesFactory& Factory;
    TResolver& Resolver;
    std::function<TSocket(TAddress&)> SocketFactory;
    THostPort HostPort;
    std::vector<char> OutputBuffer;
    size_t CommittedBytes = 0;
    // Bytes handed to sockets; congruent to the stream offset of OutputBuffer mod
    // MaxPayloadAlignment, also after a reconnect (see the filler in DoConnect)
    uint64_t SentBytes = 0;
    std::vector<char> SendBuffer;
};

} // namespace NActors
} // namespace NNet
