#include <chrono>
#include <iostream>
#include <vector>
#include <set>
#include <coroio/all.hpp>
#include <coroio/resolver.hpp>
#include <coroio/corochain.hpp>
#include <coroio/actors/actorsystem.hpp>

#include <cstdint>

using namespace NNet;
using namespace NNet::NActors;

template<size_t Size>
struct TPingMessage {
    static constexpr TMessageId MessageId = 100;
    char Data[Size] = {};
};

template<>
struct TPingMessage<0> {
    static constexpr TMessageId MessageId = 100;
    // Empty message, no data
};

template<size_t Size>
class TPingActor : public IActor {
public:
    TPingActor(bool isFirstNode, int total, int nextNodeId, const std::vector<TNodeId>& nodeIds)
        : IsFirstNode(isFirstNode)
        , TotalMessages(total)
        , RemainingMessages(total)
        , NextNodeId(nextNodeId)
        , NodeIds(nodeIds)
    { }

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        if (IsFirstNode && !TimerStarted) {
            std::cout << "Starting pinging...\n";
            TimerStarted = true;
            StartTime = std::chrono::steady_clock::now();
        }

        if (IsFirstNode && RemainingMessages == 0) {
            return;
        }

        auto nextActorId = TActorId{NextNodeId, ctx->Self().ActorId(), ctx->Self().Cookie()};
        ctx->Send(nextActorId, TPingMessage<Size>{});

        if (IsFirstNode) {
            if (ctx->Sender()) {
                --RemainingMessages;
            }
            PrintProgress();

            if (RemainingMessages == 0) {
                auto now = std::chrono::steady_clock::now();
                double secs = std::chrono::duration<double>(now - StartTime).count();
                std::cout << "\nPing throughput: "
                            << (double)(TotalMessages) / secs
                            << " msg/s\n";

                for (auto nodeId : NodeIds) {
                    auto actorId = TActorId{nodeId, ctx->Self().ActorId(), ctx->Self().Cookie()};
                    std::cerr << "Sending poison pill to actor: " << actorId << "\n";
                    ctx->Send(actorId, TPoison{});
                }
            }
        }
        return;
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

    bool IsFirstNode;
    bool TimerStarted = false;
    int TotalMessages;
    int RemainingMessages;
    TNodeId NextNodeId;
    std::chrono::steady_clock::time_point StartTime;
    int LastPercent_ = -1;
    std::vector<TNodeId> NodeIds;
};

int main(int argc, char** argv) {
    //using Poller = TSelect;
    using Poller = TDefaultPoller;
    TInitializer init;
    TLoop<Poller> loop;
    TResolver<TPollerBase> resolver(loop.Poller());
    TMessagesFactory factory;
    int messageSize = 0;
    std::set<int> allowedMessageSizes = {0, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    std::vector<
        std::tuple<int, std::unique_ptr<TNode<Poller, TResolver<TPollerBase>>>>
    > nodes;
    TNodeId myNodeId = 1;
    int port = 0;
    int delay = 5000;
    int inflight = 2;
    int messages = 100000;

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
        } else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            delay = std::stoi(argv[++i]);
        } else if (!strcmp(argv[i], "--inflight") && i + 1 < argc) {
            inflight = std::stoi(argv[++i]);
        } else if (!strcmp(argv[i], "--messages") && i + 1 < argc) {
            messages = std::stoi(argv[++i]);
        } else if (!strcmp(argv[i], "--message-size") && i + 1 < argc) {
            messageSize = std::stoi(argv[++i]);
            if (allowedMessageSizes.find(messageSize) == allowedMessageSizes.end()) {
                std::cerr << "Invalid message size. Allowed sizes: ";
                for (int size : allowedMessageSizes) {
                    std::cerr << size << " ";
                }
                std::cerr << "\n";
                return -1;
            }
        } else if (!strcmp(argv[i], "--help")) {
            std::cout << "Usage: " << argv[0] << " [--node host:port:nodeId] [--node-id id] [--delay ms] [--inflight n] [--messages n] [--message-size size]\n";
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

    TAddress address{"::", port};
    Poller::TSocket socket(loop.Poller(), address.Domain());
    socket.Bind(address);
    socket.Listen();

    sys.Serve(std::move(socket));

    auto runner = [&]<size_t MessageSize>() {
        using TPingMessageType = TPingMessage<MessageSize>;
        using TPingActorType = TPingActor<MessageSize>;

        factory.RegisterSerializer<TPingMessageType>();
        auto pingActor = std::make_unique<TPingActorType>(myNodeId == nodeIds.front(), messages, nextNodeId, nodeIds);
        auto pingActorId = sys.Register(std::move(pingActor));

        if (myNodeId == nodeIds.front()) {
            auto firstPing = [&]() -> TVoidTask {
                auto from = TActorId{0, 0, 0};
                auto to = TActorId{myNodeId, pingActorId.ActorId(), pingActorId.Cookie()};
                auto maxPings = inflight;
                auto& currentSys = sys;
                co_await sys.Sleep(std::chrono::milliseconds(delay));
                std::cerr << "Sending first ping from: " << from.ToString() << " to: " << to.ToString() << "\n";
                for (int i = 0; i < maxPings; ++i) {
                    currentSys.Send(from, to, TPingMessageType{});
                }
                co_return;
            }();
        }
    };

    switch (messageSize) {
    case 0: runner.template operator()<0>(); break;
    case 8: runner.template operator()<8>(); break;
    case 16: runner.template operator()<16>(); break;
    case 32: runner.template operator()<32>(); break;
    case 64: runner.template operator()<64>(); break;
    case 128: runner.template operator()<128>(); break;
    case 256: runner.template operator()<256>(); break;
    case 512: runner.template operator()<512>(); break;
    case 1024: runner.template operator()<1024>(); break;
    case 2048: runner.template operator()<2048>(); break;
    case 4096: runner.template operator()<4096>(); break;
    default:
        std::cerr << "Unsupported message size: " << messageSize << "\n";
        return -1;
    }

    while (sys.ActorsSize() > 0) {
        loop.Step();
    }
}
