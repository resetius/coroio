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
    TPingActor(bool isFirstNode, int total, int nextNodeId, const std::vector<uint64_t>& nodeIds)
        : IsFirstNode(isFirstNode)
        , TotalMessages(total)
        , RemainingMessages(total)
        , NextNodeId(nextNodeId)
        , NodeIds(nodeIds)
    { }

    TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) override {
        //std::cerr << "Received message of type " << message->MessageId << " from: " << ctx->Sender().ToString() << ", message: " << Counter++ << "\n";

        if (IsFirstNode && RemainingMessages == TotalMessages) {
            std::cout << "Starting pinging...\n";
            StartTime = std::chrono::steady_clock::now();
        }

        if (IsFirstNode && RemainingMessages == 0) {
            co_return;
        }

        auto nextActorId = TActorId{NextNodeId, ctx->Self().ActorId(), ctx->Self().Cookie()};
        auto next = std::make_unique<TPingMessage>();
        ctx->Send(nextActorId, std::move(next));

        // co_await ctx->Sleep(std::chrono::milliseconds(1000));
        if (IsFirstNode) {
            --RemainingMessages;
            PrintProgress();

            if (RemainingMessages == 0) {
                auto now = std::chrono::steady_clock::now();
                double secs = std::chrono::duration<double>(now - StartTime).count();
                std::cout << "\nPing throughput: "
                            << (double)(TotalMessages) / secs
                            << " msg/s\n";

                for (auto nodeId : NodeIds) {
                    auto poison = std::make_unique<TPoisonPill>();
                    auto actorId = TActorId{nodeId, ctx->Self().ActorId(), ctx->Self().Cookie()};
                    std::cerr << "Sending poison pill to actor: " << actorId << "\n";
                    ctx->Send(actorId, std::move(poison));
                }
            }
        }
        co_return;
    }

    void PrintProgress() {
        size_t processed = TotalMessages - RemainingMessages;
        int percent = int((processed * 100) / TotalMessages);
        if (percent != LastPercent_) {
            LastPercent_ = percent;
            const int barWidth = 50;
            int pos = (percent * barWidth) / 100;
            std::cerr << "\r[";
            for (int i = 0; i < barWidth; ++i) {
                if (i < pos) {
                    std::cerr << "=";
                }
                else if (i == pos) {
                    std::cerr << ">";
                }
                else {
                    std::cerr << " ";
                }
            }
            std::cerr << "] " << percent << "%";
            std::cerr.flush();
        }
    }

    int Counter = 1;
    bool IsFirstNode;
    int TotalMessages;
    int RemainingMessages;
    uint64_t NextNodeId;
    std::chrono::steady_clock::time_point StartTime;
    int LastPercent_ = -1;
    std::vector<uint64_t> NodeIds;
};

int main(int argc, char** argv) {
    //using Poller = TSelect;
    using Poller = TDefaultPoller;
    TInitializer init;
    TLoop<Poller> loop;
    TResolver<TPollerBase> resolver(loop.Poller());

    std::vector<
        std::tuple<int, std::unique_ptr<TNode<Poller::TSocket, TResolver<TPollerBase>>>>
    > nodes;
    uint64_t myNodeId = 1;
    int port = 0;
    int delay = 5000;

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

    std::vector<uint64_t> nodeIds;
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
    uint64_t nextNodeId = nodeIds[nextIdx];

    auto pingActor = std::make_unique<TPingActor>(myNodeId == nodeIds.front(), 100000, nextNodeId, nodeIds);
    auto pingActorId = sys.Register(std::move(pingActor));

    TAddress address{"::", port};
    Poller::TSocket socket(loop.Poller(), address.Domain());
    socket.Bind(address);
    socket.Listen();

    sys.Serve(std::move(socket));

    if (myNodeId == nodeIds.front()) {
        auto firstPing = [&]() -> TVoidTask {
            auto from = TActorId{nodeIds.back(), pingActorId.ActorId(), pingActorId.Cookie()};
            auto to = TActorId{myNodeId, pingActorId.ActorId(), pingActorId.Cookie()};
            co_await sys.Sleep(std::chrono::milliseconds(delay));
            std::cerr << "Sending first ping from: " << from.ToString() << " to: " << to.ToString() << "\n";
            auto pingMessage = std::make_unique<TPingMessage>();
            sys.Send(from, to, std::move(pingMessage));
            co_return;
        }();
    }

    while (sys.ActorsSize() > 0) {
        loop.Step();
    }
}
