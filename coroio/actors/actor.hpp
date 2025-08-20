#pragma once

#include <memory>
#include <coroio/corochain.hpp>
#include "messages.hpp"

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
 *             // Process message
 *             ctx->Send(ctx->Sender(), ResponseMessage{});
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
 *
 *             // Async operations
 *             co_await ctx->Sleep(std::chrono::seconds(1));
 *
 *             auto response = co_await ctx->Ask<ResponseMessage>(
 *                 someActor, QueryMessage{}
 *             );
 *
 *             ctx->Send(ctx->Sender(), response);
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
 *         // This method returns a future, so it will be handled asynchronously
 *         co_await ctx->Sleep(std::chrono::milliseconds(100));
 *
 *         auto result = co_await processAsync(msg.data);
 *         ctx->Send(ctx->Sender(), ResultMessage{result});
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

/// Local actor identifier within a node
using TLocalActorId = uint32_t;

/// Node identifier in a distributed system
using TNodeId = uint16_t;

/// Cookie for actor versioning and disambiguation
using TCookie = uint16_t;

/// Message type identifier
using TMessageId = uint32_t;

/**
 * @brief Unique identifier for actors in the system
 *
 * TActorId combines node ID, local actor ID, and cookie to create
 * a globally unique identifier for actors. The cookie helps with
 * actor lifecycle management and prevents message delivery to
 * reused actor IDs.
 */
class TActorId {
public:
    /** @brief Default constructor creates an invalid actor ID */
    TActorId() = default;

    /**
     * @brief Check if the actor ID is valid
     * @return true if the actor ID is valid (not all zeros)
     */
    operator bool() const {
        return !((NodeId_ == 0) & (ActorId_ == 0) & (Cookie_ == 0));
    }

    /** @brief Get the node ID component */
    TNodeId NodeId() const {
        return NodeId_;
    }

    /** @brief Get the local actor ID component */
    /** @brief Get the local actor ID component */
    TLocalActorId ActorId() const {
        return ActorId_;
    }

    /** @brief Get the cookie component */
    TCookie Cookie() const {
        return Cookie_;
    }

    /**
     * @brief Convert actor ID to string representation
     * @return String in format "ActorId:NodeId:LocalActorId:Cookie"
     */
    std::string ToString() const {
        return "ActorId:"
                + std::to_string(NodeId_) + ":"
                + std::to_string(ActorId_) + ":"
                + std::to_string(Cookie_);
    }

    /**
     * @brief Construct actor ID with specific components
     * @param nodeId Node identifier
     * @param actorId Local actor identifier
     * @param cookie Cookie for versioning
     */
    TActorId(TNodeId nodeId, TLocalActorId actorId, TCookie cookie)
        : NodeId_(nodeId)
        , ActorId_(actorId)
        , Cookie_(cookie)
    { }

private:
    TLocalActorId ActorId_ = 0;  ///< Local actor identifier
    TNodeId NodeId_ = 0;         ///< Node identifier
    TCookie Cookie_ = 0;         ///< Cookie for versioning
};

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
 * TActorContext is passed to actors when they receive messages. It provides
 * methods for sending messages, scheduling future messages, sleeping, and
 * performing request-response patterns.
 *
 * The context contains information about the current message sender and
 * the actor's own ID. It also manages asynchronous operations through
 * the actor system.
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
     * @brief Send a typed message to another actor
     * @tparam T Message type that has MessageId static member
     * @param to Recipient actor ID
     * @param args passed to Message constructor
     */
    template<typename T, typename... Args>
    void Send(TActorId to, Args&&... args);

    /**
     * @brief Forward a typed message to another actor
     * @tparam T Message type that has MessageId static member
     * @param to Recipient actor ID
     * @param args passed to Message constructor
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
     * @brief Sleep until a specific time
     * @param until Time to sleep until
     * @return Future that completes when the sleep time is reached
     */
    TFuture<void> Sleep(TTime until);

    /**
     * @brief Sleep for a specific duration
     * @tparam Rep Duration representation type
     * @tparam Period Duration period type
     * @param duration How long to sleep
     * @return Future that completes when the sleep duration has elapsed
     */
    template<typename Rep, typename Period>
    TFuture<void> Sleep(std::chrono::duration<Rep,Period> duration);

    /**
     * @brief Send a message and wait for a response
     * @tparam T Expected response message type
     * @tparam TQuestion Question message type
     * @param recipient Actor to send the question to
     * @param question Question message to send
     * @return Future containing the response message
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
 * ICoroActor extends IActor to support asynchronous message processing
 * using coroutines. The CoReceive method can perform async operations
 * like sleeping, waiting for responses, or doing I/O without blocking
 * the actor system.
 */
class ICoroActor : public IActor {
public:
    /**
     * @brief Synchronous receive method (calls CoReceive internally)
     * @param messageId Type identifier of the message
     * @param blob Serialized message data
     * @param ctx Actor context for communication
     */
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override;

    /**
     * @brief Asynchronous message processing method
     * @param messageId Type identifier of the message
     * @param blob Serialized message data
     * @param ctx Actor context for communication
     * @return Future that completes when message processing is done
     *
     * This method can use co_await to perform asynchronous operations
     * without blocking the actor system thread.
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
 * Usage example:
 * @code
 * class MyBehavior : public TBehavior<MyBehavior, MessageA, MessageB> {
 * public:
 *     void Receive(MessageA&& msg, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle MessageA synchronously
 *     }
 *
 *     TFuture<void> Receive(MessageB&& msg, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle MessageB asynchronously
 *         co_await someAsyncOperation();
 *     }
 *
 *     void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle messages not in the template parameter list
 *     }
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
 * IBehaviorActor allows actors to change their message handling behavior
 * dynamically at runtime. This is useful for implementing state machines,
 * protocol handlers, or any actor that needs to change its behavior based
 * on its current state.
 *
 * Usage example:
 * @code
 * class StatefulActor : public IBehaviorActor,
 *                      public TBehavior<StatefulActor, InitMessage, WorkMessage> {
 * public:
 *     StatefulActor() {
 *         Become(this); // Set initial behavior to self
 *     }
 *
 *     void Receive(InitMessage&& msg, TBlob blob, TActorContext::TPtr ctx) {
 *         // Initialize and potentially switch to a different behavior
 *         if (msg.mode == "advanced") {
 *             Become(&advancedBehavior_);
 *         }
 *     }
 *
 *     void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
 *         // Handle unknown messages
 *     }
 *
 * private:
 *     AdvancedBehavior advancedBehavior_;
 * };
 * @endcode
 */
class IBehaviorActor : public IActor {
public:
    /**
     * @brief Switch to a new behavior
     * @param behavior Pointer to the new behavior to use
     *
     * After calling Become(), all subsequent messages will be handled
     * by the new behavior until Become() is called again.
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
