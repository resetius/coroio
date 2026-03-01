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
 * @brief Globally unique identifier for actors across a distributed system
 *
 * Combines a 16-bit `NodeId`, a 32-bit local `ActorId`, and a 16-bit `Cookie`
 * into a compact, copyable handle. The cookie increments each time a slot is
 * reused, preventing stale messages from reaching a newly spawned actor that
 * occupies the same local slot as a terminated one.
 *
 * A default-constructed `TActorId` (all zeros) is invalid; `operator bool()`
 * returns `false` for it.
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
    TLocalActorId ActorId() const {
        return ActorId_;
    }

    /** @brief Get the cookie component */
    TCookie Cookie() const {
        return Cookie_;
    }

    /**
     * @brief Convert actor ID to a human-readable string
     * @return String in format `"ActorId:<NodeId>:<LocalActorId>:<Cookie>"`
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
 * @brief Wire header prepended to every remote actor message
 *
 * Written as a fixed-size prefix before the serialized payload.
 * `Size` is the byte length of the payload that follows.
 */
struct THeader {
    TActorId Sender;       ///< Originating actor
    TActorId Recipient;    ///< Destination actor
    TMessageId MessageId = 0; ///< Message type discriminator
    uint32_t Size = 0;        ///< Payload size in bytes
};

} // namespace NActors
} // namespace NNet