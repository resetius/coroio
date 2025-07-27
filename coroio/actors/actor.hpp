#pragma once

#include <memory>
#include <coroio/corochain.hpp>
#include "messages.hpp"

namespace NNet {
namespace NActors {

class TActorSystem;

class TActorId {
public:
    TActorId() = default;

    operator bool() const {
        return !((NodeId_ == 0) & (ActorId_ == 0) & (Cookie_ == 0));
    }

    uint64_t NodeId() const {
        return NodeId_;
    }

    uint64_t ActorId() const {
        return ActorId_;
    }

    uint64_t Cookie() const {
        return Cookie_;
    }

    std::string ToString() const {
        return "ActorId:"
                + std::to_string(NodeId_) + ":"
                + std::to_string(ActorId_) + ":"
                + std::to_string(Cookie_);
    }

    TActorId(uint64_t nodeId, uint64_t actorId, uint64_t cookie)
        : NodeId_(nodeId)
        , ActorId_(actorId)
        , Cookie_(cookie)
    { }

private:
    uint64_t NodeId_ = 0;
    uint64_t ActorId_ = 0;
    uint64_t Cookie_ = 0;
};

class TEnvelope
{
public:
    TActorId Sender;
    TActorId Recipient;
    uint32_t MessageId;
    TBlob Blob;
};

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
    void Send(TActorId to, uint32_t messageId, TBlob blob);
    void Forward(TActorId to, uint32_t messageId, TBlob blob);
    template<typename T>
    void Send(TActorId to, T&& message);
    template<typename T>
    void Forward(TActorId to, T&& message);

    TFuture<void> Sleep(TTime until);
    template<typename Rep, typename Period>
    TFuture<void> Sleep(std::chrono::duration<Rep,Period> duration);
    template<typename T, typename TQuestion>
    TFuture<T> Ask(TActorId recipient, TQuestion&& question);

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

    virtual TFuture<void> Receive(uint32_t messageId, TBlob blob, TActorContext::TPtr ctx) = 0;
};

} // namespace NActors
} // namespace NNet
