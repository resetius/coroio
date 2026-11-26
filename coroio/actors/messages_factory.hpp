#pragma once

#include "messages.hpp"

#include <vector>

namespace NNet {
namespace NActors {

/**
 * @brief Serializers and deserializers of message types, by message id.
 *
 * `RegisterSerializer<T>` lets a node write `T` to the wire; `RegisterDeserializer<T>`
 * lets an inbound connection build `T` from it on its own thread (`TThreadedInbound`).
 * Register every type before the threads that use the factory start: the lookups are
 * const and may then run on any thread.
 */
class TMessagesFactory {
public:
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
            if constexpr (!is_pod_v<T>) {
                if (Writers_.size() <= messageId) {
                    Writers_.resize(messageId + 1);
                }
                Writers_[messageId] = {
                    +[](const TBlob& blob) {
                        return FarPayloadSize(*static_cast<const T*>(blob.Data.get()));
                    },
                    +[](const TBlob& blob, std::span<char> out) {
                        SerializeTo(*static_cast<const T*>(blob.Data.get()), out);
                    },
                };
            }
        }
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

    /// True for a registered non-POD type: its Near objects can be written straight into a frame.
    bool HasPayloadWriter(uint32_t messageId) const {
        return messageId < Writers_.size() && Writers_[messageId].Size;
    }

    /// `FarPayloadSize` of the object in the Near `blob`; requires `HasPayloadWriter(messageId)`.
    uint32_t PayloadSize(uint32_t messageId, const TBlob& blob) const {
        return Writers_[messageId].Size(blob);
    }

    /// `SerializeTo` of the object in the Near `blob` into `out`, which has its `PayloadSize`.
    void WritePayload(uint32_t messageId, const TBlob& blob, std::span<char> out) const {
        Writers_[messageId].Write(blob, out);
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
    struct TWriter {
        uint32_t (*Size)(const TBlob&) = nullptr;
        void (*Write)(const TBlob&, std::span<char>) = nullptr;
    };
    std::vector<THandler> Handlers_;
    // Apart from Handlers_, so the code of TNode that reads it does not change
    std::vector<TWriter> Writers_;
    std::vector<TBlob(*)(TBlob&&)> Readers_;
};

} // namespace NActors
} // namespace NNet
