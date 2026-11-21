#pragma once

#include "messages.hpp"

#include <vector>

namespace NNet {
namespace NActors {

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
        }
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

private:
    struct THandler {
        TBlob(*Serialize)(TBlob&&) = nullptr;
        size_t PayloadAlignment = 1;
    };
    std::vector<THandler> Handlers_;
};

} // namespace NActors
} // namespace NNet
