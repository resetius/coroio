#pragma once

#include "actor.hpp"
#include "envelope_reader.hpp"
#include "node.hpp"
#include "queue.hpp"

#include <coroio/arena.hpp>
#include <coroio/sockutils.hpp>

#include <stack>

#ifdef Yield
#undef Yield
#endif

namespace NNet {
namespace NActors {

enum class ESystemMessages : TMessageId {
    PoisonPill = 1
};

struct TPoison {
    static constexpr TMessageId MessageId = static_cast<TMessageId>(ESystemMessages::PoisonPill);
};

template<typename T>
class TAskState
{
public:
    THandle Handle = nullptr;
    TMessageId MessageId = 0;
    TBlob Blob;
};

struct TActorInternalState
{
    TCookie Cookie = 0;
    std::unique_ptr<TUnboundedVectorQueue<TEnvelope>> Mailbox;
    TFuture<void> Pending;
    IActor::TPtr Actor;

    struct TFlags {
        uint32_t IsReady : 1 = 0; // Is the actor exists in ReadyActors queue
    } Flags = {};
};

template<typename T>
class TAsk : public IActor {
public:
    TAsk(const std::shared_ptr<TAskState<T>>& state)
        : State(state)
    { }

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override;

private:
    std::shared_ptr<TAskState<T>> State;
};

class TActorSystem
{
public:
    TActorSystem(TPollerBase* poller, int nodeId = 1)
        : Poller(poller)
        , NodeId_(nodeId)
    { }

    ~TActorSystem();

    TActorId Register(IActor::TPtr actor);

    auto Sleep(TTime until) {
        return Poller->Sleep(until);
    }

    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        return Poller->Sleep(duration);
    }

    void Send(TActorId sender, TActorId recepient, TMessageId messageId, TBlob blob);
    template<typename T>
    void Send(TActorId sender, TActorId recepient, T&& message) {
        auto blob = SerializeNear(std::forward<T>(message), GetPodAllocator());
        Send(sender, recepient, T::MessageId, std::move(blob));
    }

    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob);
    template<typename T>
    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, T&& message) {
        auto blob = SerializeNear(std::forward<T>(message), GetPodAllocator());
        return Schedule(when, sender, recipient, T::MessageId, std::move(blob));
    }
    void Cancel(TEvent event);

    template<typename T, typename TQuestion>
    auto Ask(TActorId recepient, TQuestion&& message) {
        class TAskAwaiter
        {
        public:
            TAskAwaiter(const std::shared_ptr<TAskState<T>>& state)
                : State(state)
            { }

            bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(THandle h) {
                State->Handle = h;
            }

            T await_resume() {
                if (T::MessageId != State->MessageId) {
                    throw std::runtime_error("MessageId mismatch in Ask awaiter");
                }

                return DeserializeNear<T>(State->Blob);
            }

        private:
            std::shared_ptr<TAskState<T>> State;
        };

        auto state = std::make_shared<TAskState<T>>();
        auto askActor = std::make_unique<TAsk<T>>(state);
        auto actorId = Register(std::move(askActor));
        Send(actorId, recepient, TQuestion::MessageId, SerializeNear(std::forward<TQuestion>(message), GetPodAllocator()));
        return TAskAwaiter{state};
    }

    void YieldNotify();

    size_t ActorsSize() const;

    void AddNode(int id, std::unique_ptr<INode> node);

    // Use Serve() for local actors and Serve(TSocket) for local and remote actors
    void Serve();

    template<typename TSocket>
    void Serve(TSocket socket) {
        Serve();
        Handles.emplace_back(InboundServe(std::move(socket)));
        for (int i = 0; i < static_cast<int>(Nodes.size()); ++i) {
            if (Nodes[i].Node) {
                Handles.emplace_back(OutboundServe(i));
            }
        }
    }

private:
    void GcIterationSync();
    void ExecuteSync();
    void DrainReadyNodes();
    void AddPendingFuture(TLocalActorId id, TFuture<void>&& future);

    TVoidTask OutboundServe(int id) {
        Nodes[id].Node->StartConnect();
        while (true) {
            co_await SuspendExecution(id);
            auto& node = Nodes[id].Node;
            if (node) {
                node->Drain();
            } else {
                std::cerr << "Node with id: " << id << " is not registered\n";
            }
        }
    }

    template<typename TSocket>
    TVoidTask InboundServe(TSocket socket) {
        std::cerr << "InboundServe started\n";
        while (true) {
            auto client = co_await socket.Accept();
            std::cerr << "Accepted\n";
            InboundConnection(std::move(client));
        }
        co_return;
    }

    template<typename TSocket>
    TVoidTask InboundConnection(TSocket socket) {
        static constexpr size_t ReadSize = 512 * 1024;
        static constexpr size_t InflightBytes = 16 * 1024 * 1024;
        static constexpr size_t MaxBytesBeforeYield = 2 * 1024 * 1024;
        std::vector<char> buffer(ReadSize);
        TEnvelopeReader envelopeReader;
        uint64_t message = 0;

        try {
            while (true) {
                if (envelopeReader.Size() < InflightBytes || envelopeReader.NeedMoreData()) {
                    auto size = co_await socket.ReadSome(buffer.data(), ReadSize);
                    if (size < 0) {
                        continue;
                    }
                    if (size == 0) {
                        throw std::runtime_error("Socket closed");
                    }

                    envelopeReader.Push(buffer.data(), size);
                }

                size_t bytesProcessed = 0;
                while (auto envelope = envelopeReader.Pop()) {
                    if (envelope->Recipient.NodeId() != NodeId_) [[unlikely]] {
                        std::cerr << "Received message for different node: " << envelope->Recipient.ToString() << "\n";
                        continue;
                    }

                    bytesProcessed += envelope->Blob.Size + sizeof(TSendData);
                    Send(envelope->Sender, envelope->Recipient, envelope->MessageId, std::move(envelope->Blob));
                    if (bytesProcessed >= MaxBytesBeforeYield) {
                        co_await Poller->Yield();
                        break;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in InboundConnection: " << e.what() << "\n";
        }
    }

    void ShutdownActor(TLocalActorId actorId) {
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

    TFuture<void> SuspendExecution(int nodeId) {
        Nodes[nodeId].Pending = co_await Self();
        co_await std::suspend_always();
        Nodes[nodeId].Pending = {};
        co_return;
    }

    // TODO: rewrite
    struct TPodAllocator {
        void* Acquire(size_t size) {
            return ::operator new(size);
        }

        void Release(void* ptr) {
            ::operator delete(ptr);
        }
    };

    TPodAllocator& GetPodAllocator() {
        return PodAllocator;
    }

    TPollerBase* Poller;

    TUnboundedVectorQueue<TLocalActorId> ReadyActors;
    std::vector<TActorInternalState> Actors;
    int AliveActors = 0;

    std::vector<TFuture<void>> CleanupMessages;
    std::stack<TLocalActorId, std::vector<TLocalActorId>> FreeActorIds;

    TArenaAllocator<TActorContext> ContextAllocator;

    TPodAllocator PodAllocator;

    TLocalActorId NextActorId_ = 1;
    TCookie NextCookie_ = 1;
    TNodeId NodeId_ = 1;
    THandle YieldCoroutine_{};
    THandle ScheduleCoroutine_{};
    bool IsYielding_ = true;

    struct TNodeState {
        std::unique_ptr<INode> Node;
        THandle Pending;
        struct TFlags {
            uint32_t IsReady : 1 = 0;
        } Flags = {};
    };
    std::vector<TNodeState> Nodes;
    TUnboundedVectorQueue<TNodeId> ReadyNodes;

    struct TDelayed {
        TTime When;
        unsigned TimerId = 0;
        bool valid = true;
        TActorId Sender;
        TActorId Recipient;
        TMessageId MessageId;
        TBlob Blob;

        bool operator<(const TDelayed& other) const {
            return std::tie(When, TimerId, valid) > std::tie(other.When, other.TimerId, other.valid);
        }
    };

    std::priority_queue<TDelayed> DelayedMessages;

    std::vector<THandle> Handles;

    friend class TActorContext;
};

template<typename T>
void TAsk<T>::Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
    State->MessageId = messageId;
    State->Blob = std::move(blob);
    State->Handle.resume();
    ctx->Send(ctx->Self(), TPoison{});
}

inline TFuture<void> TActorContext::Sleep(TTime until) {
    co_return co_await ActorSystem->Sleep(until);
}

template<typename Rep, typename Period>
inline TFuture<void> TActorContext::Sleep(std::chrono::duration<Rep,Period> duration) {
    co_return co_await ActorSystem->Sleep(duration);
}

template<typename T, typename TQuestion>
inline TFuture<T> TActorContext::Ask(TActorId recipient, TQuestion&& question) {
    co_return co_await ActorSystem->Ask<T>(recipient, std::forward<TQuestion>(question));
}

template<typename T>
inline void TActorContext::Send(TActorId to, T&& message) {
    auto blob = SerializeNear(std::forward<T>(message), ActorSystem->GetPodAllocator());
    Send(to, T::MessageId, std::move(blob));
}
template<typename T>
inline void TActorContext::Forward(TActorId to, T&& message) {
    auto blob = SerializeNear(std::forward<T>(message), ActorSystem->GetPodAllocator());
    Forward(to, T::MessageId, std::move(blob));
}

template<typename T>
inline TEvent TActorContext::Schedule(TTime when, TActorId sender, TActorId recipient, T&& message) {
    auto blob = SerializeNear(std::forward<T>(message), ActorSystem->GetPodAllocator());
    return Schedule(when, sender, recipient, T::MessageId, std::move(blob));
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

