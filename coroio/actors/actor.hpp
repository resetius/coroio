#pragma once

#include <memory>
#include <coroio/corochain.hpp>
#include "messages.hpp"
#include "actorid.hpp"

/**
 * @file actor.hpp
 * @brief Actor system implementation with message passing and behavior support
 *
 * This file contains the core components of an actor-based concurrent system.
 * Actors are lightweight, isolated units of computation that communicate through
 * message passing. The system supports both synchronous and asynchronous message
 * handling, behavior switching, and coroutine-based actors.
 *
 * @section usage Basic Usage Examples
 *
 * @subsection simple_actor Simple Actor Example
 * @code
 * class MyActor : public IActor {
 * public:
 *     void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
 *         if (messageId == MyMessage::MessageId) {
 *             auto message = DeserializeNear<MyMessage>(blob);
 *             // Process message and reply to the sender
 *             ctx->Send<ResponseMessage>(ctx->Sender(), message.value * 2);
 *         }
 *     }
 * };
 * @endcode
 *
 * @subsection coro_actor Coroutine Actor Example
 * @code
 * class MyCoroActor : public ICoroActor {
 * public:
 *     TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
 *         if (messageId == MyMessage::MessageId) {
 *             auto message = DeserializeNear<MyMessage>(blob);
 *             auto sender = ctx->Sender();
 *
 *             // Async sleep — mailbox is paused until this handler finishes
 *             co_await ctx->Sleep(std::chrono::seconds(1));
 *
 *             // Request-reply: blocks this handler, not the event loop
 *             auto response = co_await ctx->Ask<ResponseMessage>(
 *                 someActor, QueryMessage{}
 *             );
 *
 *             ctx->Send<ResponseMessage>(sender, std::move(response));
 *         }
 *     }
 * };
 * @endcode
 *
 * @subsection behavior_actor Behavior-Based Actor Example
 * @code
 * class MyBehaviorActor : public IBehaviorActor,
 *                        public TBehavior<MyBehaviorActor, StartMessage, DataMessage> {
 * public:
 *     MyBehaviorActor() {
 *         Become(this); // Set initial behavior
 *     }
 *
 *     void Receive(StartMessage&& msg, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle start message
 *         state_ = State::Started;
 *     }
 *
 *     void Receive(DataMessage&& msg, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle data message
 *         processData(msg.data);
 *     }
 *
 *     void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle unknown messages
 *     }
 *
 * private:
 *     enum class State { Idle, Started };
 *     State state_ = State::Idle;
 * };
 * @endcode
 *
 * @subsection async_behavior Async Behavior Example
 * @code
 * class AsyncBehaviorActor : public IBehaviorActor,
 *                          public TBehavior<AsyncBehaviorActor, ProcessMessage> {
 * public:
 *     AsyncBehaviorActor() { Become(this); }
 *
 *     TFuture<void> Receive(ProcessMessage&& msg, TBlob blob, TActorContext::TPtr ctx) {
 *         // TFuture<void> return type triggers async dispatch automatically
 *         co_await ctx->Sleep(std::chrono::milliseconds(100));
 *
 *         auto result = co_await processAsync(msg.data);
 *         ctx->Send<ResultMessage>(ctx->Sender(), std::move(result));
 *     }
 *
 *     void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle unknown messages
 *     }
 * };
 * @endcode
 */

namespace NNet {
namespace NActors {

class TActorSystem;

/**
 * @brief Message envelope containing routing and payload information
 *
 * TEnvelope wraps messages with sender/recipient information and
 * contains the serialized message data.
 */
class TEnvelope
{
public:
    TActorId Sender;        ///< Actor that sent the message
    TActorId Recipient;     ///< Actor that should receive the message
    TMessageId MessageId;   ///< Type identifier of the message
    TBlob Blob;            ///< Serialized message data
};

using TEvent = std::pair<unsigned, TTime>;

/**
 * @brief Context object providing actor communication and scheduling capabilities
 *
 * TActorContext is passed to actors during message dispatch. It provides
 * methods for sending messages, scheduling future messages, sleeping, and
 * performing request-response patterns.
 *
 * The context is valid only for the duration of the `Receive` / `CoReceive`
 * call chain. Capture `ctx->Sender()` / `ctx->Self()` by value if you need
 * them after the first `co_await`.
 *
 * @note `Sleep`, `Ask`, and any coroutine-based methods require `co_await`
 *       and are only meaningful inside `ICoroActor::CoReceive` or an async
 *       `TBehavior::Receive` (one returning `TFuture<void>`).
 */
class TActorContext
{
public:
    using TPtr = std::unique_ptr<TActorContext>;

    /** @brief Get the sender of the current message */
    TActorId Sender() const {
        return Sender_;
    }

    /** @brief Get this actor's ID */
    TActorId Self() const {
        return Self_;
    }

    /**
     * @brief Send a message to another actor
     * @param to Recipient actor ID
     * @param messageId Message type identifier
     * @param blob Serialized message data
     */
    void Send(TActorId to, TMessageId messageId, TBlob blob);

    /**
     * @brief Forward a message to another actor (preserves original sender)
     * @param to Recipient actor ID
     * @param messageId Message type identifier
     * @param blob Serialized message data
     */
    void Forward(TActorId to, TMessageId messageId, TBlob blob);

    /**
     * @brief Send a typed message to another actor (non-blocking)
     *
     * Constructs a `T` in-place from `args` and enqueues it to the recipient.
     * Returns immediately; delivery is deferred to the next event-loop iteration.
     *
     * @tparam T Message type; must have a `static TMessageId MessageId` member
     * @param to Recipient actor ID
     * @param args Constructor arguments forwarded to `T`
     *
     * @code
     * ctx->Send<Pong>(ctx->Sender(), 42);   // Pong{42} delivered to sender
     * @endcode
     */
    template<typename T, typename... Args>
    void Send(TActorId to, Args&&... args);

    /**
     * @brief Forward a typed message to another actor, preserving the original sender
     *
     * Like `Send<T>`, but the recipient sees `ctx->Sender()` equal to the
     * sender of the *current* message rather than this actor's ID. Useful for
     * routing/proxy actors that should not appear in the reply chain.
     *
     * @tparam T Message type; must have a `static TMessageId MessageId` member
     * @param to Recipient actor ID
     * @param args Constructor arguments forwarded to `T`
     */
    template<typename T, typename... Args>
    void Forward(TActorId to, Args&&... args);

    /**
     * @brief Schedule a message to be delivered at a specific time
     * @param when Time when the message should be delivered
     * @param sender Sender actor ID for the scheduled message
     * @param recipient Recipient actor ID
     * @param messageId Message type identifier
     * @param blob Serialized message data
     * @return Event handle that can be used to cancel the scheduled message
     */
    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob);

    /**
     * @brief Schedule a typed message to be delivered at a specific time
     * @tparam T Message type that has MessageId static member
     * @param when Time when the message should be delivered
     * @param sender Sender actor ID for the scheduled message
     * @param recipient Recipient actor ID
     * @param args args passed to the Message constructor
     * @return Event handle that can be used to cancel the scheduled message
     */
    template<typename T, typename... Args>
    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, Args&&... args);

    /**
     * @brief Cancel a previously scheduled message
     * @param event Event handle returned by Schedule()
     */
    void Cancel(TEvent event);

    /**
     * @brief Suspend the current handler until a specific time
     *
     * Must be `co_await`-ed inside `ICoroActor::CoReceive` or an async
     * `TBehavior::Receive`. The actor system continues processing other
     * actors while this handler is suspended.
     *
     * @param until Absolute time point to wake up at
     * @return Awaitable future that resumes at `until`
     */
    TFuture<void> Sleep(TTime until);

    /**
     * @brief Suspend the current handler for a duration
     *
     * Convenience overload for relative delays. Must be `co_await`-ed.
     *
     * @code
     * co_await ctx->Sleep(std::chrono::seconds(5));
     * @endcode
     *
     * @tparam Rep   Duration representation type
     * @tparam Period Duration period type
     * @param duration How long to sleep
     * @return Awaitable future that resumes after `duration`
     */
    template<typename Rep, typename Period>
    TFuture<void> Sleep(std::chrono::duration<Rep,Period> duration);

    /**
     * @brief Send a request and suspend until a reply of type `T` arrives
     *
     * Sends `question` to `recipient` and returns a future that resolves to the
     * first reply of type `T` sent back to this actor. Must be `co_await`-ed
     * inside `ICoroActor::CoReceive` or an async `TBehavior::Receive`.
     *
     * @code
     * auto reply = co_await ctx->Ask<Pong>(pingActor, Ping{});
     * @endcode
     *
     * @tparam T       Expected response message type
     * @tparam TQuestion Question message type
     * @param recipient Actor to send the question to
     * @param question  Message forwarded to the recipient
     * @return Awaitable future resolving to `T` when the reply arrives
     */
    template<typename T, typename TQuestion>
    TFuture<T> Ask(TActorId recipient, TQuestion&& question);

    /**
     * @brief Helper class for managing asynchronous operations in actors
     *
     * TAsync allows actors to start asynchronous operations that will
     * continue running even after the actor's Receive method returns.
     * This is essential for non-blocking actor implementations.
     */
    struct TAsync {
        TAsync(TActorSystem* actorSystem, TLocalActorId actorId)
            : ActorSystem_(actorSystem)
            , ActorId_(actorId)
        { }

        void Commit(TFuture<void>&& future);

    private:
        TLocalActorId ActorId_;
        TActorSystem* ActorSystem_;
    };

    /**
     * @brief Start an asynchronous operation context
     * @return TAsync helper for managing the async operation
     */
    TAsync StartAsync();

    static void* operator new(size_t size, TActorSystem* actorSystem);
    static void operator delete(void* ptr);

private:
    TActorContext(TActorId sender, TActorId self, TActorSystem* actorSystem)
        : Sender_(sender)
        , Self_(self)
        , ActorSystem(actorSystem)
    { }

    TActorId Sender_;              ///< Sender of the current message
    TActorId Self_;                ///< This actor's ID
    TActorSystem* ActorSystem = nullptr;  ///< Pointer to the actor system

    friend class TActorSystem;
    friend class TMockActorContext;
};

/**
 * @brief Mock actor context for testing purposes
 *
 * TMockActorContext provides a testable version of TActorContext
 * that can be constructed directly for unit testing actors.
 */
class TMockActorContext : public TActorContext {
public:
    TMockActorContext(TActorId sender, TActorId self, TActorSystem* actorSystem)
        : TActorContext(sender, self, actorSystem)
    { }
};

/**
 * @brief Base interface for all actors in the system
 *
 * IActor defines the basic contract that all actors must implement.
 * Actors receive messages through the Receive method and can respond
 * by sending messages back through the provided context.
 *
 * This is the synchronous version of actor interface - the Receive
 * method should complete quickly and not block on I/O operations.
 */
class IActor {
public:
    using TPtr = std::unique_ptr<IActor>;
    friend class TActorSystem;

    IActor() = default;
    virtual ~IActor() = default;

    /**
     * @brief Process an incoming message
     * @param messageId Type identifier of the message
     * @param blob Serialized message data
     * @param ctx Actor context for sending responses and accessing actor system
     *
     * This method should process the message quickly and not perform
     * blocking operations. For async operations, use
     * ICoroActor instead.
     */
    virtual void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) = 0;
};

/**
 * @brief Coroutine-based actor interface for asynchronous message processing
 *
 * Extends `IActor` so that `CoReceive` can `co_await` timers, `Ask`, or I/O
 * without blocking the event loop thread.
 *
 * **One async handler at a time.** When `CoReceive` returns a pending
 * `TFuture<void>`, the actor's mailbox is paused: no further messages are
 * dispatched until that future completes. This removes the need for internal
 * locking — state can be mutated freely inside the handler.
 *
 * @code
 * class Throttled : public ICoroActor {
 *     TFuture<void> CoReceive(TMessageId id, TBlob blob, TActorContext::TPtr ctx) override {
 *         auto msg = DeserializeNear<Work>(blob);
 *         co_await ctx->Sleep(std::chrono::milliseconds(50)); // next msg waits here
 *         process(msg);
 *     }
 * };
 * @endcode
 */
class ICoroActor : public IActor {
public:
    /**
     * @brief IActor bridge — invokes CoReceive and parks pending futures
     *
     * Called by the actor system. Not meant to be overridden; override
     * `CoReceive` instead.
     */
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override;

    /**
     * @brief Asynchronous message handler (override in subclass)
     *
     * May freely use `co_await ctx->Sleep(...)`, `co_await ctx->Ask<T>(...)`,
     * or any other coroutine primitive. While this coroutine is suspended,
     * the actor's mailbox is paused — subsequent messages queue up and are
     * delivered only after this handler returns (i.e. its future completes).
     *
     * @param messageId Type identifier of the incoming message
     * @param blob      Serialized message payload
     * @param ctx       Actor context (valid for the duration of this handler)
     * @return Future that signals completion of message processing
     */
    virtual TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) = 0;
};

/**
 * @brief Base interface for actor behaviors
 *
 * IBehavior defines a pluggable message handling strategy that can be
 * switched at runtime. This allows actors to change their behavior
 * dynamically based on their current state or external conditions.
 */
class IBehavior {
public:
    virtual ~IBehavior() = default;

    /**
     * @brief Process an incoming message according to this behavior
     * @param messageId Type identifier of the message
     * @param blob Serialized message data
     * @param ctx Actor context for communication
     */
    virtual void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) = 0;
};

/**
 * @brief Template for type-safe behavior implementations
 * @tparam TBaseBehavior The concrete behavior class that inherits from this template
 * @tparam TMessages List of message types this behavior can handle
 *
 * TBehavior provides automatic message deserialization and type-safe
 * message handling. It supports both synchronous and asynchronous
 * message handlers, automatically detecting the return type.
 *
 * The handler return type determines dispatch mode automatically:
 * - `void`           → synchronous, called inline
 * - `TFuture<void>`  → asynchronous, mailbox pauses until the future completes
 *
 * Usage example:
 * @code
 * class MyBehavior : public TBehavior<MyBehavior, Ping, Upload> {
 * public:
 *     void Receive(Ping&& msg, TBlob, TActorContext::TPtr ctx) {
 *         ctx->Send<Pong>(ctx->Sender());          // synchronous reply
 *     }
 *
 *     TFuture<void> Receive(Upload&& msg, TBlob, TActorContext::TPtr ctx) {
 *         auto sender = ctx->Sender();
 *         co_await ctx->Sleep(std::chrono::seconds(1)); // async — mailbox pauses
 *         ctx->Send<UploadDone>(sender);
 *     }
 *
 *     void HandleUnknownMessage(TMessageId id, TBlob, TActorContext::TPtr) {}
 * };
 * @endcode
 */
template<typename TBaseBehavior, typename... TMessages>
class TBehavior : public IBehavior
{
public:
    /**
     * @brief Process incoming message with automatic type dispatch
     * @param messageId Type identifier of the message
     * @param blob Serialized message data
     * @param ctx Actor context for communication
     *
     * This method automatically deserializes the message and calls the
     * appropriate typed Receive method on the derived class.
     */
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        bool handled = (TryHandleMessage<TMessages>(
            messageId,
            blob,
            ctx
        ) || ...);

        if (!handled) {
            static_cast<TBaseBehavior*>(this)->HandleUnknownMessage(messageId, std::move(blob), std::move(ctx));
        }
    }

private:
    template<typename TMessage>
    bool TryHandleMessage(TMessageId messageId, TBlob& blob, TActorContext::TPtr& ctx) {
        if (TMessage::MessageId == messageId) {
            if (blob.Type == TBlob::PointerType::Near) {
                auto&& mes = DeserializeNear<TMessage>(blob);
                HandleMessage<TMessage>(
                    std::move(mes),
                    std::move(blob),
                    std::move(ctx)
                );
            } else {
                auto&& mes = DeserializeFar<TMessage>(blob);
                HandleMessage<TMessage>(
                    std::move(mes),
                    std::move(blob),
                    std::move(ctx)
                );
            }
            return true;
        }
        return false;
    }

    template<typename TMessage>
    void HandleMessage(TMessage&& message, TBlob blob, TActorContext::TPtr ctx)
    {
        using ReturnType = decltype(static_cast<TBaseBehavior*>(this)->Receive(
            std::declval<TMessage>(),
            std::declval<TBlob>(),
            std::declval<TActorContext::TPtr>()
        ));

        if constexpr (std::is_same_v<ReturnType, void>) {
            static_cast<TBaseBehavior*>(this)->Receive(
                std::move(message),
                std::move(blob),
                std::move(ctx)
            );
        } else {
            auto async = ctx->StartAsync();
            auto future = static_cast<TBaseBehavior*>(this)->Receive(
                std::move(message),
                std::move(blob),
                std::move(ctx)
            );
            if (!future.done()) {
                async.Commit(std::move(future));
            }
        }
    }
};

/**
 * @brief Actor that delegates message handling to a pluggable behavior
 *
 * Enables runtime behavior switching — useful for state machines and protocol
 * handlers. Most commonly the actor itself is the initial behavior (`Become(this)`),
 * but separate `TBehavior` objects can be stored as members and swapped in.
 *
 * Usage example:
 * @code
 * struct IdleBehavior;
 * struct ActiveBehavior;
 *
 * class Connection : public IBehaviorActor,
 *                    public TBehavior<Connection, Connect, Data, Disconnect> {
 * public:
 *     Connection() { Become(this); }
 *
 *     void Receive(Connect&&, TBlob, TActorContext::TPtr ctx) {
 *         connected_ = true;
 *         // next message will already use this actor as behavior (no switch needed)
 *     }
 *
 *     void Receive(Data&& msg, TBlob, TActorContext::TPtr ctx) {
 *         if (!connected_) return;
 *         process(msg.payload);
 *     }
 *
 *     void Receive(Disconnect&&, TBlob, TActorContext::TPtr) {
 *         connected_ = false;
 *     }
 *
 *     void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {}
 *
 * private:
 *     bool connected_ = false;
 * };
 * @endcode
 */
class IBehaviorActor : public IActor {
public:
    /**
     * @brief Switch to a new behavior
     *
     * The switch takes effect on the **next** incoming message. The behavior
     * pointer must remain valid for the lifetime of the actor — storing
     * behaviors as member variables is the typical pattern.
     *
     * @param behavior Non-owning pointer to the new behavior; must not be null
     */
    void Become(IBehavior* behavior) {
        CurrentBehavior_ = behavior;
    }

    /**
     * @brief Delegate message handling to the current behavior
     * @param messageId Type identifier of the message
     * @param blob Serialized message data
     * @param ctx Actor context for communication
     */
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        CurrentBehavior_->Receive(messageId, std::move(blob), std::move(ctx));
    }

private:
    IBehavior* CurrentBehavior_;
};

} // namespace NActors
} // namespace NNet
