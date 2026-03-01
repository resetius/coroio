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
    /// Reserves `size` contiguous bytes in the buffer and returns a span over them.
    virtual std::span<char> Acquire(size_t size) = 0;
    /// Marks `size` previously acquired bytes as ready to send.
    virtual void Commit(size_t size) = 0;
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
class TNode : public INode {
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

    // TODO: remove me, envelope needed only for local actors
    void Send(TEnvelope&& envelope) override {
        auto blob = std::move(envelope.Blob);
        if (blob.Size > 0) {
            blob = Factory.SerializeFar(envelope.MessageId, std::move(blob));
        }
        THeader data{
            .Sender = envelope.Sender,
            .Recipient = envelope.Recipient,
            .MessageId = envelope.MessageId,
            .Size = blob.Size
        };
        auto buf = Acquire(sizeof(data) + blob.Size);
        std::memcpy(buf.data(), &data, sizeof(data));
        if (blob.Size > 0) {
            std::memcpy(buf.data() + sizeof(data), blob.Data.get(), blob.Size);
        }
        Commit(sizeof(data) + blob.Size);
    }

    std::span<char> Acquire(size_t size) override {
        if (UncommittedBytes + size > OutputBuffer.size()) {
            OutputBuffer.resize(UncommittedBytes + size);
        }
        auto buf = std::span<char>(OutputBuffer.data() + UncommittedBytes, size);
        UncommittedBytes += size;
        return buf;
    }

    void Commit(size_t size) override {
        CommittedBytes += size;
        UncommittedBytes = CommittedBytes;
    }

    void StartConnect() override {
        if (!Connected) {
            Connect();
            return;
        }
    }

    void Drain() override {
        StartConnect();
        if (Connected && (!Drainer.raw() || Drainer.done())) {
            Drainer = DoDrain();
        }
    }

    THostPort GetHostPort() const override {
        return HostPort;
    }

private:
    TFuture<void> DoDrain() {
        try {
            while (CommittedBytes > 0) {
                SendBuffer.swap(OutputBuffer);
                auto readSize = CommittedBytes;
                UncommittedBytes = CommittedBytes = 0;
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
                co_await Socket.Connect(addr /*, deadline*/);
                // Connection bug workaround:
                {
                    auto sender = TActorId(0, 0, 0);
                    auto recipient = TActorId(0, 0, 0);
                    THeader data{
                        .Sender = sender,
                        .Recipient = recipient,
                        .MessageId = 0
                    };
                    co_await TByteWriter(Socket).Write(&data, sizeof(data));
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
    size_t UncommittedBytes = 0;
    size_t CommittedBytes = 0;
    std::vector<char> SendBuffer;
};

} // namespace NActors
} // namespace NNet
