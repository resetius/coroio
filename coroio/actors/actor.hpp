#pragma once

#include <memory>
#include <coroio/corochain.hpp>

namespace NNet {
namespace NActors {

class TActorSystem;

class TActorId {
public:
    TActorId() = default;
    
    bool operator() const {
        return (NodeId == 0) & (ActorId == 0);
    }

private:
    uint64_t NodeId = 0;
    uint64_t ActorId = 0;
};

class TMessage
{
public:
    using TPtr = TMessage*;

    uint64_t MessageId;
    TActorId From;
    TActorId To;
};

class IActor {
public:
    using TPtr = std::unique_ptr<IActor>;
    friend class TActorSystem;

    IActor() = default;
    ~IActor() = default;

    virtual TFuture<void> Receive(TMessage::TPtr message) = 0;

private:
    void Attach(TActorSystem* actorSystem, TActorId actorId);

protected:
    TActorId SelfActorId;
    TActorSystem* ActorSystem = nullptr;
};

} // namespace NActors
} // namespace NNet
