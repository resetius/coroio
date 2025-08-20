#pragma once

namespace NNet {
namespace NActors {

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
 * @brief Header for messages sent between actors.
 * Used in remote communication and serialization.
 */
struct THeader {
    TActorId Sender;
    TActorId Recipient;
    TMessageId MessageId = 0;
    uint32_t Size = 0;
};

} // namespace NActors
} // namespace NNet