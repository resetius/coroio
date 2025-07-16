#include <chrono>
#include <iostream>
#include <vector>
#include <coroio/all.hpp>
#include <coroio/resolver.hpp>
#include <coroio/corochain.hpp>
#include <coroio/actors/actorsystem.hpp>

#include <cstdint>

using namespace NNet;
using namespace NNet::NActors;

class TPingMessage : public TMessage {
public:
    TPingMessage() {
        MessageId = 10;
    }
};

class TPongMessage : public TMessage {
public:
    TPongMessage() {
        MessageId = 20;
    }
};

class TPingActor : public IActor {
public:
    TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) override {
        std::cerr << "Received message of type " << message->MessageId << " from: " << ctx->Sender().ToString() << ", message: " << counter++ << "\n";

        if (message->MessageId == 10) {
            auto reply = std::make_unique<TPongMessage>();
            ctx->Send(ctx->Sender(), std::move(reply));
        } else if (message->MessageId == 20) {
            auto reply = std::make_unique<TPingMessage>();
            auto sender = ctx->Sender();
            ctx->Send(ctx->Sender(), std::move(reply));
        }

        co_await ctx->Sleep(std::chrono::milliseconds(1000));
        co_return;
    }

    int counter = 1;
};

int main(int argc, char** argv) {
    using Poller = TSelect;
    TInitializer init;
    TLoop<Poller> loop;
    TResolver<TPollerBase> resolver(loop.Poller());

    std::vector<
        std::tuple<int, std::unique_ptr<TNode<Poller::TSocket, TResolver<TPollerBase>>>>
    > nodes;
    int myNodeId = 1;
    int port = 0;
    int delay = 1000;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--node") && i + 1 < argc) {
            // Parse host:port:nodeId
            std::string hostPortNode(argv[++i]);
            size_t colon1 = hostPortNode.find(':');
            size_t colon2 = hostPortNode.find(':', colon1 + 1);
            if (colon1 != std::string::npos && colon2 != std::string::npos) {
                std::string host = hostPortNode.substr(0, colon1);
                int port = std::stoi(hostPortNode.substr(colon1 + 1, colon2 - colon1 - 1));
                int nodeId = std::stoi(hostPortNode.substr(colon2 + 1));
                nodes.emplace_back(nodeId, std::make_unique<TNode<Poller::TSocket, TResolver<TPollerBase>>>(
                    resolver,
                    [&](const TAddress& addr) {
                        return Poller::TSocket(loop.Poller(), addr.Domain());
                    },
                    THostPort(host, port)
                ));
                std::cerr << "Node: " << host << ":" << port << " with id: " << nodeId << "\n";
            } else {
                std::cerr << "Invalid node format. Use host:port:nodeId\n";
                return -1;
            }
        } else if (!strcmp(argv[i], "--node-id") && i + 1 < argc) {
            myNodeId = std::stoi(argv[++i]);
        } else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            delay = std::stoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {
            std::cout << "Usage: " << argv[0] << " [--node host:port:nodeId] [--node-id id] [--delay ms]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            return -1;
        }
    }

    TActorSystem sys(&loop.Poller(), myNodeId);
    for (auto&& [nodeId, node] : nodes) {
        if (nodeId != myNodeId) {
            std::cerr << "Adding node with id: " << nodeId << "\n";
            sys.AddNode(nodeId, std::move(node));
        } else {
            port = node->GetHostPort().GetPort();
        }
    }

    auto pingActor = std::make_unique<TPingActor>();
    auto pingActorId = sys.Register(std::move(pingActor));

    TAddress address{"::", port};
    Poller::TSocket socket(loop.Poller(), address.Domain());
    socket.Bind(address);
    socket.Listen();

    sys.Serve(std::move(socket));

    auto firstPing = [&]() -> TVoidTask {
        if (myNodeId == 1) {
            auto from = TActorId{2, pingActorId.ActorId(), pingActorId.Cookie()};
            auto to = pingActorId;
            co_await sys.Sleep(std::chrono::milliseconds(delay));
            std::cerr << "Sending first ping from: " << from.ToString() << " to: " << to.ToString() << "\n";
            auto pingMessage = std::make_unique<TPingMessage>();
            sys.Send(from, to, std::move(pingMessage));
        }
        co_return;
    }();

    loop.Loop();
}
