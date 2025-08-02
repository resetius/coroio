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

struct TMessage {
    static constexpr TMessageId MessageId = 100;
    std::string Text;
};

struct TPing {
    static constexpr TMessageId MessageId = 101;
};

namespace NNet::NActors {

template<>
void SerializeToStream<TMessage>(const TMessage& obj, std::ostringstream& oss) {
    oss << obj.Text;
}

template<>
void DeserializeFromStream<TMessage>(TMessage& obj, std::istringstream& iss) {
    obj.Text = iss.str();
}

} // namespace NNet::NActors

struct TBehaviorActor : public IBehaviorActor {
    struct TRichBehavior1 : public TBehavior<TRichBehavior1, TPing, TMessage> {
        TRichBehavior1(TBehaviorActor* parent)
            : Parent(parent)
        { }

        void Receive(TPing&& message, TBlob blob, TActorContext::TPtr ctx) {
            TMessage msg{"hello"};

            std::cout << "Sending message: " << msg.Text << "\n";
            auto nodeId = Parent->NodeIds.front();
            if (Parent->NodeIds.size() > 1) {
                nodeId = Parent->NodeIds[1 + rand() % (Parent->NodeIds.size() - 1)];
            }
            auto nextActorId = TActorId{nodeId, ctx->Self().ActorId(), ctx->Self().Cookie()};
            ctx->Schedule(std::chrono::steady_clock::now() + std::chrono::milliseconds(1000), ctx->Self(), ctx->Self(), TPing{});
            ctx->Send(nextActorId, std::move(msg));
        }
        void Receive(TMessage&& message, TBlob blob, TActorContext::TPtr ctx) {
            std::cout << "Received message: " << message.Text << "\n";
        }
        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
            std::cerr << "Unknown message received: " << messageId << "\n";
        }

        TBehaviorActor* Parent;
    };

    TBehaviorActor(int myIdx, const std::vector<TNodeId>& nodeIds)
        : NodeIds(nodeIds)
    {
        std::swap(NodeIds[myIdx], NodeIds[0]);
        Become(std::move(std::make_unique<TRichBehavior1>(this)));
    }

    std::vector<TNodeId> NodeIds;
};

int main(int argc, char** argv) {
    //using Poller = TSelect;
    using Poller = TDefaultPoller;
    TInitializer init;
    TLoop<Poller> loop;
    TResolver<TPollerBase> resolver(loop.Poller());
    TMessagesFactory factory;

    factory.RegisterSerializer<TMessage>();
    factory.RegisterSerializer<TPing>();

    std::vector<
        std::tuple<int, std::unique_ptr<TNode<Poller, TResolver<TPollerBase>>>>
    > nodes;
    TNodeId myNodeId = 1;
    int port = 0;

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
                nodes.emplace_back(nodeId, std::make_unique<TNode<Poller, TResolver<TPollerBase>>>(
                    loop.Poller(),
                    factory,
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
        } else if (!strcmp(argv[i], "--help")) {
            std::cout << "Usage: " << argv[0] << " [--node host:port:nodeId] [--node-id id]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            return -1;
        }
    }

    std::vector<TNodeId> nodeIds;
    TActorSystem sys(&loop.Poller(), myNodeId);
    for (auto&& [nodeId, node] : nodes) {
        if (nodeId != myNodeId) {
            std::cerr << "Adding node with id: " << nodeId << "\n";
            sys.AddNode(nodeId, std::move(node));
        } else {
            port = node->GetHostPort().GetPort();
        }
        nodeIds.push_back(nodeId);
    }
    std::sort(nodeIds.begin(), nodeIds.end());
    auto it = std::find(nodeIds.begin(), nodeIds.end(), myNodeId);
    if (it == nodeIds.end()) {
        std::cerr << "Node with id: " << myNodeId << " is not registered.\n";
        return -1;
    }
    int myIdx = std::distance(nodeIds.begin(), it);
    int nextIdx = (myIdx + 1) % nodeIds.size();
    TNodeId nextNodeId = nodeIds[nextIdx];

    auto pingActor = std::make_unique<TBehaviorActor>(myIdx, nodeIds);
    auto pingActorId = sys.Register(std::move(pingActor));

    TAddress address{"::", port};
    Poller::TSocket socket(loop.Poller(), address.Domain());
    socket.Bind(address);
    socket.Listen();

    sys.Serve(std::move(socket));
    sys.Send(TActorId{}, pingActorId, TPing{});

    while (sys.ActorsSize() > 0) {
        loop.Step();
    }
}
