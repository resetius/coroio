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
        return (NodeId_ == 0) & (ActorId_ == 0);
    }

    uint64_t NodeId() const {
        return NodeId_;
    }

    uint64_t ActorId() const {
        return ActorId_;
    }

    std::string ToString() const {
        return "ActorId:" + std::to_string(NodeId_) + ":" + std::to_string(ActorId_);
    }

private:
    friend class TActorSystem;
    TActorId(uint64_t nodeId, uint64_t actorId)
        : NodeId_(nodeId)
        , ActorId_(actorId)
    { }

    uint64_t NodeId_ = 0;
    uint64_t ActorId_ = 0;
};

class TMessage
{
public:
    using TPtr = std::unique_ptr<TMessage>;

    uint64_t MessageId;
    TActorId From;
    TActorId To;
};

class IActor {
public:
    using TPtr = std::unique_ptr<IActor>;
    friend class TActorSystem;

    IActor() = default;
    virtual ~IActor() = default;

    virtual TFuture<void> Receive(TMessage::TPtr message) = 0;

private:
    void Attach(TActorSystem* actorSystem, TActorId actorId) {
        SelfActorId = actorId;
        ActorSystem = actorSystem;
    }

protected:
    // TODO: store only shortId
    TActorId SelfActorId;
    TActorSystem* ActorSystem = nullptr;
};

} // namespace NActors
} // namespace NNet
