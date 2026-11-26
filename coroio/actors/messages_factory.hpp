#pragma once

#include "messages.hpp"

#include <vector>

namespace NNet {
namespace NActors {

/**
 * @brief Serializers and deserializers of message types, by message id.
 *
 * - `RegisterSerializer<T>` lets a node write `T` to the wire.
 * - `RegisterPayloadWriter<T>` also lets a `TThreadedNode` write the payloads of Near
 *   objects of `T` on its I/O thread; the I/O thread does not read the factory for it.
 * - `RegisterDeserializer<T>` lets a `TThreadedInbound` build `T` on its I/O thread.
 *
 * Registration and the lookups of the nodes run on the thread of the actor system.
 * Register the deserializers before the inbound threads start: they read the factory.
 */
class TMessagesFactory {
public:
    /// Writes the payload of the object in a Near blob into `out`, which has its size.
    using TPayloadWrite = void (*)(const TBlob& blob, std::span<char> out) noexcept;

    /// The payload writer of a type, see `RegisterPayloadWriter`.
    struct TPayloadWriter {
        /// `FarPayloadSize` of the object in a Near blob; may throw.
        uint32_t (*Size)(const TBlob& blob) = nullptr;
        /// `SerializeTo` of that object, which must not throw.
        TPayloadWrite Write = nullptr;
    };

    // Local messages have no wire handler, even when registered generically.
    template<typename T>
    void RegisterSerializer() {
        if constexpr (!is_local_message_v<T>) {
            constexpr auto messageId = T::MessageId;
            static_assert(messageId <= MaxMessageId, "the top bits of THeader::MessageId hold the payload padding");
            static_assert(messageId != FillerMessageId, "the id of the filler frame is reserved");
            if (Handlers_.size() <= messageId) {
                Handlers_.resize(messageId + 1);
            }
            auto serializeFunc = +[](TBlob&& blob) {
                return ::NNet::NActors::SerializeFar<T>(std::move(blob));
            };
            Handlers_[messageId] = {serializeFunc, payload_alignment_v<T>};
        }
    }

    /**
     * @brief `RegisterSerializer<T>`, and a `TThreadedNode` writes the payloads of Near objects of `T` on its I/O thread.
     *
     * The node takes `SerializedSize` in `Send` and calls `SerializeTo` later, on its I/O
     * thread, where it then destroys the object. So for `T`:
     * - what `SerializeTo` reads must not change, and must stay alive, until the object is destroyed;
     * - the destructor of the object and the deleter of its blob must be safe on the I/O thread;
     * - `SerializeTo` must not throw: check the object in `SerializedSize`, which throws to the
     *   sender. An exception from `SerializeTo` terminates the process.
     */
    template<typename T>
    void RegisterPayloadWriter() {
        static_assert(!is_local_message_v<T>, "local messages never go to the wire");
        static_assert(!is_pod_v<T>, "a POD payload costs as much to queue as to copy");
        RegisterSerializer<T>();
        constexpr auto messageId = T::MessageId;
        if (Writers_.size() <= messageId) {
            Writers_.resize(messageId + 1);
        }
        Writers_[messageId] = {
            +[](const TBlob& blob) {
                return FarPayloadSize(*static_cast<const T*>(blob.Data.get()));
            },
            +[](const TBlob& blob, std::span<char> out) noexcept {
                SerializeTo(*static_cast<const T*>(blob.Data.get()), out);
            },
        };
    }

    /**
     * @brief Registers the conversion of a Far blob of `T` into a Near one, see `DeserializeFar`.
     *
     * For non-POD types with `DeserializeFrom<T>`: a POD payload is read in place already.
     */
    template<typename T>
    void RegisterDeserializer() {
        static_assert(!is_local_message_v<T>, "local messages never come from the wire");
        static_assert(!is_pod_v<T>, "a POD payload is read in place, as a Near blob would be");
        constexpr auto messageId = T::MessageId;
        if (Readers_.size() <= messageId) {
            Readers_.resize(messageId + 1);
        }
        Readers_[messageId] = +[](TBlob&& blob) {
            auto obj = std::make_unique<T>();
            DeserializeFrom(*obj, blob);
            return TBlob{TBlob::TRawPtr(obj.release(), TBlobDeleter{[](void* ptr) {
                delete static_cast<T*>(ptr);
            }}), sizeof(T), TBlob::PointerType::Near};
        };
    }

    TBlob SerializeFar(uint32_t messageId, TBlob blob) {
        if (messageId >= Handlers_.size() || !Handlers_[messageId].Serialize) [[unlikely]] {
            throw std::runtime_error(
                std::string("No handler for message ID: ") + std::to_string(messageId)
            );
        }
        return Handlers_[messageId].Serialize(std::move(blob));
    }

    /// `PayloadAlignment` of the type registered for `messageId`, 1 if there is none.
    size_t PayloadAlignment(uint32_t messageId) const {
        return messageId < Handlers_.size() ? Handlers_[messageId].PayloadAlignment : 1;
    }

    /// The writer registered with `RegisterPayloadWriter` for `messageId`, or nullptr.
    /// The pointer is valid until the next registration.
    const TPayloadWriter* PayloadWriter(uint32_t messageId) const {
        return messageId < Writers_.size() && Writers_[messageId].Size ? &Writers_[messageId] : nullptr;
    }

    /**
     * @brief Builds the object of a Far `blob` with `DeserializeFrom` and returns it as a Near blob.
     *
     * Returns `blob` unchanged if no deserializer is registered for `messageId`. The object
     * may keep the bytes of `blob`, as with `DeserializeFar<T>`; otherwise they are freed here.
     */
    TBlob DeserializeFar(uint32_t messageId, TBlob blob) const {
        if (messageId >= Readers_.size() || !Readers_[messageId]) {
            return blob;
        }
        return Readers_[messageId](std::move(blob));
    }

private:
    struct THandler {
        TBlob(*Serialize)(TBlob&&) = nullptr;
        size_t PayloadAlignment = 1;
    };
    std::vector<THandler> Handlers_;
    // Apart from Handlers_, so the code of TNode that reads it does not change
    std::vector<TPayloadWriter> Writers_;
    std::vector<TBlob(*)(TBlob&&)> Readers_;
};

} // namespace NActors
} // namespace NNet
