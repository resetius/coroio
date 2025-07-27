#pragma once

#include "messages.hpp"

#include <vector>
#include <functional>

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
        auto serializeFunc = [](TBlob&& blob) {
            return ::NNet::NActors::SerializeFar<T>(std::move(blob));
        };
        Handlers_[messageId] = std::move(serializeFunc);
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
#if __cpp_lib_move_only_function >= 202110L
    std::vector<std::move_only_function<TBlob(TBlob&&)>> Handlers_;
#else
    std::vector<std::function<TBlob(TBlob&&)>> Handlers_;
#endif
};

} // namespace NActors
} // namespace NNet
