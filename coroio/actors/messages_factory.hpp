#pragma once

#include "messages.hpp"

#include <vector>

namespace NNet {
namespace NActors {

class TMessagesFactory {
public:
    template<typename T>
    void RegisterSerializer() {
        constexpr auto messageId = T::MessageId;
        if (Handlers_.size() <= messageId) {
            Handlers_.resize(messageId + 1);
        }
        auto serializeFunc = +[](TBlob&& blob) {
            return ::NNet::NActors::SerializeFar<T>(std::move(blob));
        };
        Handlers_[messageId] = serializeFunc;
    }

    TBlob SerializeFar(uint32_t messageId, TBlob blob) {
        if (messageId >= Handlers_.size() || !Handlers_[messageId]) [[unlikely]] {
            throw std::runtime_error(
                std::string("No handler for message ID: ") + std::to_string(messageId)
            );
        }
        return Handlers_[messageId](std::move(blob));
    }

private:
    std::vector<TBlob(*)(TBlob&&)> Handlers_;
};

} // namespace NActors
} // namespace NNet
