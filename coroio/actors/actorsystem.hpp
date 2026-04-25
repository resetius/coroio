#pragma once

#include "actor.hpp"
#include "envelope_reader.hpp"
#include "node.hpp"
#include "queue.hpp"

#include <coroio/arena.hpp>
#include <coroio/sockutils.hpp>

#include <functional>
#include <stack>

#ifdef Yield
#undef Yield
#endif

namespace NNet {
namespace NActors {

enum class ESystemMessages : TMessageId {
    PoisonPill = 1
};

/**
 * @brief System message that terminates the receiving actor.
 *
 * Sending `TPoison` to any actor removes it from the system. Its destructor
 * is called on the next GC cycle. The actor slot is then recycled with a
 * bumped cookie so stale messages cannot reach its successor.
 *
 * @code
 * ctx->Send<TPoison>(actorToKill);
 * @endcode
 */
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

/**
 * @brief Single-threaded actor runtime.
 *
 * Manages actor registration, message delivery, timers, and (optionally)
 * distributed communication over TCP. All actors run on the single thread
 * that drives the poller — no locking is required inside handlers.
 *
 * **Local-only setup:**
 * @code
 * TLoop<TDefaultPoller> loop;
 * TActorSystem sys(&loop.Poller(), 1);
 * auto id = sys.Register(std::make_unique<MyActor>());
 * sys.Send<StartMsg>(id, id);
 * sys.Serve();
 * loop.Loop();
 * @endcode
 *
 * **Distributed setup** — call `AddNode` for each peer, then `Serve(socket)`:
 * @code
 * TActorSystem sys(&loop.Poller(), 1);
 * sys.AddNode(2, std::make_unique<TNode<TDefaultPoller>>(loop.Poller(), TAddress{"::", 9002}));
 * sys.Serve(std::move(listeningSocket));  // inbound + outbound coroutines
 * loop.Loop();
 * @endcode
 */
class TActorSystem
{
public:
    /**
     * @brief Constructs the actor system.
     *
     * @param poller  Poller used for timers and I/O. Must outlive this object.
     * @param nodeId  This node's identifier (default 1). Must be unique across
     *                all processes in a distributed cluster.
     */
    TActorSystem(TPollerBase* poller, int nodeId = 1, std::function<void(const std::string&)> logger = {})
        : Poller(poller)
        , NodeId_(nodeId)
        , Logger_(std::move(logger))
    { }

    ~TActorSystem();

    /**
     * @brief Registers an actor and returns its system-wide ID.
     *
     * Takes ownership of the actor. The returned `TActorId` is valid until
     * `TPoison` is delivered or the system is destroyed. Slots are reused
     * with a bumped cookie to prevent stale delivery.
     *
     * @param actor Unique pointer to the actor (ownership transferred).
     * @return Globally unique ID for this actor.
     */
    TActorId Register(IActor::TPtr actor);

    /// Suspends the current coroutine until `until`. Delegates to the poller.
    auto Sleep(TTime until) {
        return Poller->Sleep(until);
    }

    /// Suspends the current coroutine for `duration`. Delegates to the poller.
    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        return Poller->Sleep(duration);
    }

    /// Low-level send with a pre-serialized blob. Prefer the typed `Send<T>` overload.
    void Send(TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob);

    /**
     * @brief Sends a typed message from `sender` to `recipient` (non-blocking).
     *
     * Routes automatically: local actors receive via their mailbox; remote
     * actors (different `NodeId`) are serialized and queued to the outbound
     * node buffer. Delivery happens on the next event-loop iteration.
     *
     * @tparam T Message type; must have `static TMessageId MessageId`.
     * @param sender    Actor ID that appears as `ctx->Sender()` in the handler.
     * @param recipient Destination actor ID.
     * @param args      Constructor arguments forwarded to `T`.
     */
    template<typename T, typename... Args>
    void Send(TActorId sender, TActorId recipient, Args&&... args) {
        if (recipient.NodeId() == NodeId_) {
            auto blob = SerializeNear<T>(GetPodAllocator(), std::forward<Args>(args)...);
            Send(sender, recipient, T::MessageId, std::move(blob));
        } else {
            auto& maybeRemote = Nodes[recipient.NodeId()];
            if (!maybeRemote.Node) {
                if (Logger_) Logger_("Cannot send message to actor on different node: " + recipient.ToString());
                return;
            }
            SerializeFarInplace<T>(*maybeRemote.Node, sender, recipient, std::forward<Args>(args)...);
            if (maybeRemote.Flags.IsReady == 0) {
                maybeRemote.Flags.IsReady = 1;
                ReadyNodes.Push(recipient.NodeId());
            }

            YieldNotify();
        }
    }

    /// Low-level schedule with a pre-serialized blob. Prefer the typed `Schedule<T>` overload.
    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob);

    /**
     * @brief Schedules a typed message to be delivered at `when`.
     *
     * @param when      Absolute time point for delivery.
     * @param sender    Actor ID for the scheduled message.
     * @param recipient Destination actor ID.
     * @param args      Constructor arguments forwarded to `T`.
     * @return Event handle — pass to `Cancel()` to abort before delivery.
     */
    template<typename T, typename... Args>
    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, Args&&... args) {
        auto blob = SerializeNear<T>(GetPodAllocator(), std::forward<Args>(args)...);
        return Schedule(when, sender, recipient, T::MessageId, std::move(blob));
    }

    /**
     * @brief Cancels a previously scheduled message.
     *
     * Safe to call even if the message has already been delivered.
     * @param event Handle returned by `Schedule()`.
     */
    void Cancel(TEvent event);

    /**
     * @brief Sends a request and returns an awaitable for the reply of type `T`.
     *
     * Registers a temporary one-shot actor that captures the reply and
     * self-destructs via `TPoison`. Must be `co_await`-ed inside
     * `ICoroActor::CoReceive` or an async `TBehavior::Receive`.
     *
     * @tparam T         Expected reply message type.
     * @tparam TQuestion Request message type.
     * @param recepient  Actor to send the question to.
     * @param message    Request message forwarded to `Send`.
     * @return Awaitable that resolves to `T` when the reply arrives.
     *
     * @code
     * auto reply = co_await sys.Ask<Pong>(pingActor, Ping{});
     * @endcode
     */
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
        Send(actorId, recepient, TQuestion::MessageId, SerializeNear<TQuestion>(GetPodAllocator(), std::forward<TQuestion>(message)));
        return TAskAwaiter{state};
    }

    /**
     * @brief Wakes up the internal scheduler to process newly ready nodes.
     *
     * Called automatically after enqueuing a message to a remote node.
     * Not intended for direct use in application code.
     */
    void YieldNotify();

    /// Returns the number of currently registered (alive) actors.
    size_t ActorsSize() const;

    /**
     * @brief Registers a remote node for distributed message routing.
     *
     * Messages sent to an actor whose `NodeId()` equals `id` are serialized
     * and forwarded via this node's outbound buffer. Must be called before
     * `Serve(TSocket)`.
     *
     * @param id   Node identifier matching the remote process's `nodeId`.
     * @param node Transport handle (e.g. `TNode<TDefaultPoller>`).
     */
    void AddNode(int id, std::unique_ptr<INode> node);

    /**
     * @brief Starts the actor system event loop for local actors only.
     *
     * Schedules background coroutines that call `ExecuteSync` and
     * `GcIterationSync` on every poller tick. Use when there are no remote
     * nodes. For distributed setups use `Serve(TSocket)` instead.
     */
    void Serve();

    /**
     * @brief Starts the actor system with inbound and outbound network serving.
     *
     * Calls `Serve()` for local processing, then launches:
     * - An inbound accept loop on `socket` for messages from remote nodes.
     * - One outbound coroutine per node registered via `AddNode()` to drain
     *   outbound message buffers.
     *
     * @param socket Bound, listening socket for inbound connections.
     */
    template<typename TSocket, typename TEnvelopeReader = TZeroCopyEnvelopeReader>
    void Serve(TSocket socket) {
        Serve();
        Handles.emplace_back(InboundServe<TSocket, TEnvelopeReader>(std::move(socket)));
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
                if (Logger_) Logger_("Node with id: " + std::to_string(id) + " is not registered");
            }
        }
    }

    template<typename TSocket, typename TEnvelopeReader>
    TVoidTask InboundServe(TSocket socket) {
        while (true) {
            auto client = co_await socket.Accept();
            InboundConnection<TSocket, TEnvelopeReader>(std::move(client));
        }
        co_return;
    }

    template<typename TSocket, typename TEnvelopeReader>
    TVoidTask InboundConnection(TSocket socket) {
        static constexpr size_t ReadSize = 512 * 1024;
        static constexpr size_t InflightBytes = 16 * 1024 * 1024;
        static constexpr size_t MaxBytesBeforeYield = 2 * 1024 * 1024;
        TEnvelopeReader envelopeReader(InflightBytes, /*lowWatermark = */ 1024);
        uint64_t message = 0;

        try {
            while (true) {
                if (envelopeReader.Size() < InflightBytes || envelopeReader.NeedMoreData()) {
                    auto buffer = envelopeReader.Acquire(ReadSize);
                    auto size = co_await socket.ReadSome(buffer.data(), buffer.size());
                    if (size < 0) {
                        continue;
                    }
                    if (size == 0) {
                        throw std::runtime_error("Socket closed");
                    }
                    envelopeReader.Commit(size);
                }

                //envelopeReader.PrintDebugInfo();

                size_t bytesProcessed = 0;
                while (auto envelope = envelopeReader.Pop()) {
                    if (envelope->Recipient.NodeId() != NodeId_) [[unlikely]] {
                        if (Logger_) Logger_("Received message for different node: " + envelope->Recipient.ToString());
                        continue;
                    }

                    bytesProcessed += envelope->Blob.Size + sizeof(THeader);
                    Send(envelope->Sender, envelope->Recipient, envelope->MessageId, std::move(envelope->Blob));
                    if (bytesProcessed >= MaxBytesBeforeYield) {
                        break;
                    }
                }

                co_await Poller->Yield();
            }
        } catch (const std::exception& e) {
            if (Logger_) Logger_(std::string("Error in InboundConnection: ") + e.what());
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
    std::function<void(const std::string&)> Logger_;

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
    ctx->Send<TPoison>(ctx->Self());
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

template<typename T, typename... Args>
inline void TActorContext::Send(TActorId to, Args&&... args) {
    ActorSystem->Send<T>(Self(), to, std::forward<Args>(args)...);
}
template<typename T, typename... Args>
inline void TActorContext::Forward(TActorId to, Args&&... args) {
    ActorSystem->Send<T>(Sender(), to, std::forward<Args>(args)...);
}

template<typename T, typename... Args>
inline TEvent TActorContext::Schedule(TTime when, TActorId sender, TActorId recipient, Args&&... args) {
    auto blob = SerializeNear<T>(ActorSystem->GetPodAllocator(), std::forward<Args>(args)...);
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

