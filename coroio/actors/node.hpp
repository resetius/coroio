#pragma once

#include "actor.hpp"
#include "envelope_reader.hpp"
#include "messages_factory.hpp"

#include <coroio/address.hpp>
#include <coroio/resolver.hpp>

namespace NNet {
namespace NActors {

class INode {
public:
    virtual ~INode() = default;
    virtual void Send(TEnvelope&& envelope) = 0;
    virtual void StartConnect() = 0;
    virtual void Drain() = 0;
    virtual THostPort GetHostPort() const = 0;
};

template<typename TPoller, typename TResolver>
class TNode : public INode {
public:
    using TSocket = typename TPoller::TSocket;
    TNode(TPoller& poller, TMessagesFactory& factory, TResolver& resolver, const std::function<TSocket(const TAddress&)>& socketFactory, const THostPort& hostPort)
        : Poller(poller)
        , Factory(factory)
        , Resolver(resolver)
        , SocketFactory(socketFactory)
        , HostPort(hostPort)
    { }

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
        OutputBuffer.insert(OutputBuffer.end(), (char*)&data, (char*)&data + sizeof(data));
        if (blob.Size > 0) {
            OutputBuffer.insert(OutputBuffer.end(), (char*)blob.Data.get(), (char*)blob.Data.get() + blob.Size);
        }
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
            while (!OutputBuffer.empty()) {
                SendBuffer.swap(OutputBuffer);
                co_await TByteWriter(Socket).Write(SendBuffer.data(), SendBuffer.size());
                SendBuffer.clear();
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
    std::vector<char> SendBuffer;
};

} // namespace NActors
} // namespace NNet