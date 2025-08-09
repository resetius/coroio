#pragma once

#include "actor.hpp"
#include "queue.hpp"

#include <deque>
#include <span>

namespace NNet {
namespace NActors {

struct THeader {
    TActorId Sender;
    TActorId Recipient;
    TMessageId MessageId = 0;
    uint32_t Size = 0;
};

class TEnvelopeReader {
public:
    void Push(const char* buf, size_t size);

    std::optional<TEnvelope> Pop();
    bool NeedMoreData() const {
        return !Messages.empty();
    }
    size_t Size() const {
        return Buffer.size();
    }

private:
    void Process();

    bool HasHeader = false;
    THeader Header;
    std::queue<TEnvelope> Messages;
    std::deque<char> Buffer;
};

class TZeroCopyEnvelopeReader {
public:
    TZeroCopyEnvelopeReader(size_t capacity = 1024);
    std::span<char> Acquire(size_t size);
    void Commit(size_t size);
    std::optional<TEnvelope> Pop();

private:
    void EnsureCapacity();
    void Process();

    size_t Head = 0;
    size_t Tail = 0;
    size_t LastIndex = 0;

    bool HasHeader = false;
    THeader Header;
    TUnboundedVectorQueue<TEnvelope> Messages;
    std::vector<char> Data;
};

} // namespace NActors
} // namespace NNet