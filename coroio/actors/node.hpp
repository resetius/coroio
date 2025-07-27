#pragma once

#include "actor.hpp"
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

struct TSendData {
    TActorId Sender;
    TActorId Recipient;
    TMessageId MessageId;
    uint32_t Size;
};

template<typename TSocket, typename TResolver>
class TNode : public INode {
public:
    TNode(TMessagesFactory& factory, TResolver& resolver, const std::function<TSocket(const TAddress&)>& socketFactory, const THostPort& hostPort)
        : Factory(factory)
        , Resolver(resolver)
        , SocketFactory(socketFactory)
        , HostPort(hostPort)
    { }

    void Send(TEnvelope&& envelope) override {
        TSendData data{
            .Sender = envelope.Sender,
            .Recipient = envelope.Recipient,
            .MessageId = envelope.MessageId,
            .Size = envelope.Blob.Size
        };
        OutputBuffer.insert(OutputBuffer.end(), (char*)&data, (char*)&data + sizeof(data));
        if (envelope.Blob.Size > 0) {
            auto farBlob = Factory.SerializeFar(envelope.MessageId, std::move(envelope.Blob));
            OutputBuffer.insert(OutputBuffer.end(), (char*)farBlob.Data.get(), (char*)farBlob.Data.get() + farBlob.Size);
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
        if (!Drainer.raw() || Drainer.done()) {
            Drainer = DoDrain();
        }
    }

    THostPort GetHostPort() const override {
        return HostPort;
    }

private:
    TFuture<void> DoDrain() {
        try {
            if (OutputBuffer.empty()) {
                co_return;
            }
            co_await TByteWriter(Socket).Write(OutputBuffer.data(), OutputBuffer.size());
            OutputBuffer.clear();
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
                    TSendData data{
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
                co_await Socket.Poller()->Sleep(std::chrono::milliseconds(1000));
            }
        }
        co_return;
    }

    bool Connected = false;
    TFuture<void> Drainer;
    TFuture<void> Connector;
    TSocket Socket;

    TMessagesFactory& Factory;
    TResolver& Resolver;
    std::function<TSocket(TAddress&)> SocketFactory;
    THostPort HostPort;
    std::vector<char> OutputBuffer;
};

} // namespace NActors
} // namespace NNet