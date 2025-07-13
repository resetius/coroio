#pragma once

#include <memory>
#include <coroio/corochain.hpp>

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

private:
    friend class TActorSystem;
    TActorId(uint64_t nodeId, uint64_t actorId, uint64_t cookie)
        : NodeId_(nodeId)
        , ActorId_(actorId)
        , Cookie_(cookie)
    { }

    uint64_t NodeId_ = 0;
    uint64_t ActorId_ = 0;
    uint64_t Cookie_ = 0;
};

class TMessage
{
public:
    using TPtr = std::unique_ptr<TMessage>;

    uint64_t MessageId;
    TActorId From;
    TActorId To;
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
    void Send(TActorId to, TMessage::TPtr message);
    void Forward(TActorId to, TMessage::TPtr message);
    TFuture<void> PipeTo(uint64_t to, TFuture<TMessage::TPtr> future);
    TFuture<void> Sleep(TTime until);
    template<typename Rep, typename Period>
    TFuture<void> Sleep(std::chrono::duration<Rep,Period> duration);
    template<typename T>
    TFuture<std::unique_ptr<T>> Ask(TActorId recepient, TMessage::TPtr message);

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

    virtual TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) = 0;
};

} // namespace NActors
} // namespace NNet
