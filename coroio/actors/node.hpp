#pragma once

#include "actor.hpp"

#include <coroio/address.hpp>
#include <coroio/resolver.hpp>

namespace NNet {
namespace NActors {

class INode {
public:
    virtual ~INode() = default;
    virtual void Send(TEnvelope&& envelope) = 0;
    virtual void Drain() = 0;
};

template<typename TSocket, typename TResolver>
class TNode : public INode {
public:
    TNode(TResolver& resolver, const std::function<TSocket(const TAddress&)>& socketFactory, const THostPort& hostPort)
        : Resolver(resolver)
        , SocketFactory(socketFactory)
        , HostPort(hostPort)
    { }

    void Send(TEnvelope&& envelope) override {
        OutgoingMessages.push(std::move(envelope));
    }

    void Drain() override {
        if (!Connected) {
            Connect();
            return;
        }

        if (!Drainer.raw() || Drainer.done()) {
            Drainer = DoDrain();
        }
    }

private:
    TFuture<void> DoDrain() {
        // TODO: serialize data
        struct TSendData {
            TActorId Sender;
            TActorId Recipient;
            uint64_t MessageId;
        };

        try {
            while (!OutgoingMessages.empty()) {
                auto envelope = std::move(OutgoingMessages.front());
                OutgoingMessages.pop();

                // TODO: serialize
                TSendData data{
                    .Sender = envelope.Sender,
                    .Recipient = envelope.Recipient,
                    .MessageId = envelope.Message->MessageId
                };
                co_await TByteWriter(Socket).Write(&data, sizeof(data));
            }
            Connected = true;
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
                auto deadline = NNet::TClock::now() + std::chrono::milliseconds(100);
                TAddress addr = co_await HostPort.Resolve(Resolver);
                Socket = SocketFactory(addr);
                co_await Socket.Connect(deadline);
                Connected = true;
                std::cout << "Connected to " << HostPort.ToString() << "\n";
            } catch (const std::exception& ex) {
                std::cerr << "Error connecting to " << HostPort.ToString() << ": " << ex.what() << "\n";
                Connected = false;
            }
        }
        co_return;
    }

    bool Connected = false;
    TFuture<void> Drainer;
    TFuture<void> Connector;
    TSocket Socket;

    TResolver& Resolver;
    std::function<TSocket(TAddress&)> SocketFactory;
    THostPort HostPort;
    std::queue<TEnvelope> OutgoingMessages;
};

} // namespace NActors
} // namespace NNet