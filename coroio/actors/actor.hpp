#pragma once

#include <memory>
#include <coroio/corochain.hpp>
#include "messages.hpp"

namespace NNet {
namespace NActors {

class TActorSystem;

using TLocalActorId = uint32_t;
using TNodeId = uint16_t;
using TCookie = uint16_t;
using TMessageId = uint32_t;

class TActorId {
public:
    TActorId() = default;

    operator bool() const {
        return !((NodeId_ == 0) & (ActorId_ == 0) & (Cookie_ == 0));
    }

    TNodeId NodeId() const {
        return NodeId_;
    }

    TLocalActorId ActorId() const {
        return ActorId_;
    }

    TCookie Cookie() const {
        return Cookie_;
    }

    std::string ToString() const {
        return "ActorId:"
                + std::to_string(NodeId_) + ":"
                + std::to_string(ActorId_) + ":"
                + std::to_string(Cookie_);
    }

    TActorId(TNodeId nodeId, TLocalActorId actorId, TCookie cookie)
        : NodeId_(nodeId)
        , ActorId_(actorId)
        , Cookie_(cookie)
    { }

private:
    TLocalActorId ActorId_ = 0;
    TNodeId NodeId_ = 0;
    TCookie Cookie_ = 0;
};

class TEnvelope
{
public:
    TActorId Sender;
    TActorId Recipient;
    TMessageId MessageId;
    TBlob Blob;
};

using TEvent = std::pair<unsigned, TTime>;

class TActorContext
{
public:
    using TPtr = std::unique_ptr<TActorContext>;

    TActorId Sender() const {
        return Sender_;
    }
    TActorId Self() const {
        return Self_;
    }
    void Send(TActorId to, TMessageId messageId, TBlob blob);
    void Forward(TActorId to, TMessageId messageId, TBlob blob);
    template<typename T>
    void Send(TActorId to, T&& message);
    template<typename T>
    void Forward(TActorId to, T&& message);
    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob);
    template<typename T>
    TEvent Schedule(TTime when, TActorId sender, TActorId recipient, T&& message);
    void Cancel(TEvent event);

    TFuture<void> Sleep(TTime until);
    template<typename Rep, typename Period>
    TFuture<void> Sleep(std::chrono::duration<Rep,Period> duration);
    template<typename T, typename TQuestion>
    TFuture<T> Ask(TActorId recipient, TQuestion&& question);

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

    TAsync StartAsync();

    static void* operator new(size_t size, TActorSystem* actorSystem);
    static void operator delete(void* ptr);

private:
    TActorContext(TActorId sender, TActorId self, TActorSystem* actorSystem)
        : Sender_(sender)
        , Self_(self)
        , ActorSystem(actorSystem)
    { }

    TActorId Sender_;
    TActorId Self_;
    TActorSystem* ActorSystem = nullptr;

    friend class TActorSystem;
};

class IActor {
public:
    using TPtr = std::unique_ptr<IActor>;
    friend class TActorSystem;

    IActor() = default;
    virtual ~IActor() = default;

    virtual void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) = 0;
};

class ICoroActor : public IActor {
public:
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override;

    virtual TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) = 0;
};

} // namespace NActors
} // namespace NNet
