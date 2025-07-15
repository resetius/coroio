#pragma once

#include "actor.hpp"
#include "node.hpp"

#include <coroio/arena.hpp>
#include <coroio/sockutils.hpp>

#include <queue>
#include <stack>

namespace NNet {
namespace NActors {

enum class ESystemMessages : uint64_t {
    PoisonPill = 1
};

class TPoisonPill : public TMessage
{
public:
    TPoisonPill() {
        MessageId = static_cast<uint64_t>(ESystemMessages::PoisonPill);
    }
};

template<typename T>
class TAskState
{
public:
    THandle Handle = nullptr;
    std::unique_ptr<T> Answer = nullptr;
};

struct TActorInternalState
{
    uint64_t Cookie = 0;
    std::unique_ptr<std::queue<TEnvelope>> Mailbox;
    TFuture<void> Pending;
    IActor::TPtr Actor;

    struct TFlags {
        uint64_t IsReady : 1 = 0;
    } Flags = {};
};

template<typename T>
class TAsk : public IActor {
public:
    TAsk(const std::shared_ptr<TAskState<T>>& state)
        : State(state)
    { }

    TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) override;

private:
    std::shared_ptr<TAskState<T>> State;
};

class TActorSystem
{
public:
    TActorSystem(TPollerBase* poller)
        : Poller(poller)
    { }

    TActorId Register(IActor::TPtr actor);

    void Schedule(TMessage::TPtr message, TTime when);

    auto Sleep(TTime until) {
        return Poller->Sleep(until);
    }

    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        return Poller->Sleep(duration);
    }

    void Send(TActorId sender, TActorId recepient, TMessage::TPtr message);

    template<typename T>
    auto Ask(TActorId recepient, TMessage::TPtr message) {
        class TAskAwaiter
        {
        public:
            TAskAwaiter(const std::shared_ptr<TAskState<T>>& state)
                : State(state)
            { }

            bool await_ready() const noexcept {
                return State->Answer != nullptr;
            }

            void await_suspend(THandle h) {
                State->Handle = h;
            }

            std::unique_ptr<T> await_resume() {
                return std::move(State->Answer);
            }

        private:
            std::shared_ptr<TAskState<T>> State;
        };

        auto state = std::make_shared<TAskState<T>>();
        auto askActor = std::make_unique<TAsk<T>>(state);
        auto actorId = Register(std::move(askActor));
        Send(actorId, recepient, std::move(message));
        return TAskAwaiter{state};
    }

    void MaybeNotify() {
        if (ExecuteAwait_) {
            ExecuteAwait_.resume();
        }
    }

    void GcIterationSync() {
        CleanupMessages.clear();
        for (auto actorId : CleanupActors) {
            ShutdownActor(actorId);
        }
        CleanupActors.clear();
    }

    TFuture<void> WaitExecute();

    size_t ActorsSize() const {
        return AliveActors;
    }

    void AddNode(int id, std::unique_ptr<INode> node) {
        if (Nodes.size() >= id) {
            Nodes.resize(id + 1);
        }
        Nodes[id] = std::move(node);
    }

    template<typename TSocket>
    void Serve(TSocket socket) {
        InboundServe(std::move(socket));
        for (int i = 0; i < static_cast<int>(Nodes.size()); ++i) {
            if (Nodes[i]) {
                OutboundServe(i);
            }
        }
    }

private:
    TVoidTask OutboundServe(int id) {
        while (true) {
            co_await std::suspend_always{};
            auto& node = Nodes[id];
            if (node) {
                node->Drain();
            } else {
                std::cerr << "Node with id: " << id << " is not registered\n";
            }
        }
    }

    template<typename TSocket>
    TVoidTask InboundServe(TSocket socket) {
        while (true) {
            auto client = co_await socket.Accept();
            std::cerr << "Accepted\n";
            InboundConnection(std::move(client));
        }
        co_return;
    }

    template<typename TSocket>
    TVoidTask InboundConnection(TSocket socket) {
        // TODO: serialize data
        struct TSendData {
            TActorId Sender;
            TActorId Recipient;
            uint64_t MessageId;
        };

        TStructReader<TSendData, TSocket> reader(std::move(socket));

        try {
            while (true) {
                auto data = co_await reader.Read();
                if (data.Recipient.NodeId() != NodeId_) {
                    std::cerr << "Received message for different node: " << data.Recipient.ToString() << "\n";
                    continue;
                }
                auto message = std::make_unique<TMessage>();
                message->MessageId = data.MessageId;
                Send(data.Sender, data.Recipient, std::move(message));
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in InboundConnection: " << e.what() << "\n";
        }
    }

    void ShutdownActor(uint64_t actorId) {
        if (actorId < Actors.size()) {
            AliveActors--;
            Actors[actorId] = {};
            FreeActorIds.push(actorId);
        }
    }

    void* AllocateActorContext() {
        return ContextAllocator.Allocate();
    }

    void DeallocateActorContext(TActorContext* ptr) {
        ContextAllocator.Deallocate(ptr);
    }

    TPollerBase* Poller;

    std::queue<uint64_t> ReadyActors;
    std::vector<TActorInternalState> Actors;
    int AliveActors = 0;

    std::vector<TFuture<void>> CleanupMessages;
    std::vector<uint64_t> CleanupActors;
    std::stack<uint64_t> FreeActorIds;

    TArenaAllocator<TActorContext> ContextAllocator;

    uint64_t NextActorId_ = 1;
    uint64_t NextCookie_ = 1;
    uint64_t NodeId_ = 1;
    THandle ExecuteAwait_{};

    std::vector<std::unique_ptr<INode>> Nodes;

    friend class TActorContext;
};

template<typename T>
TFuture<void> TAsk<T>::Receive(TMessage::TPtr message, TActorContext::TPtr ctx) {
    State->Answer = std::unique_ptr<T>(static_cast<T*>(message.release()));
    State->Handle.resume();
    auto command = std::make_unique<TPoisonPill>();
    ctx->Send(ctx->Self(), std::move(command));
    co_return;
}

inline TFuture<void> TActorContext::Sleep(TTime until) {
    co_return co_await ActorSystem->Sleep(until);
}

template<typename Rep, typename Period>
inline TFuture<void> TActorContext::Sleep(std::chrono::duration<Rep,Period> duration) {
    co_return co_await ActorSystem->Sleep(duration);
}

template<typename T>
inline TFuture<std::unique_ptr<T>> TActorContext::Ask(TActorId recepient, TMessage::TPtr message) {
    co_return co_await ActorSystem->Ask<T>(recepient, std::move(message));
}

inline void* TActorContext::operator new(size_t size, TActorSystem* actorSystem) {
    return actorSystem->AllocateActorContext();
}

inline void TActorContext::operator delete(void* ptr) {
    if (ptr) {
        auto* self = static_cast<TActorContext*>(ptr);
        auto* actorSystem = self->ActorSystem;
        actorSystem->DeallocateActorContext(self);
    }
}

} // namespace NActors
} // namespace NNet

