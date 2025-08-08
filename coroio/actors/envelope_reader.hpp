#pragma once

#include "actor.hpp"

#include <deque>

namespace NNet {
namespace NActors {

struct TSendData {
    TActorId Sender;
    TActorId Recipient;
    TMessageId MessageId = 0;
    uint32_t Size = 0;
};

class TEnvelopeReader {
public:
    void Push(const char* buf, size_t size);

    std::optional<TEnvelope> Pop();
    bool Empty() const {
        return Messages.empty();
    }
    size_t Size() const {
        return Buffer.size();
    }

private:
    void Process();

    bool HasHeader = false;
    TSendData Header;
    std::queue<TEnvelope> Messages;
    std::deque<char> Buffer;
};

} // namespace NActors
} // namespace NNet