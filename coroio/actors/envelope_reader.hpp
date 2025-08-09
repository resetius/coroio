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
    // capacity must be a power of two
    TZeroCopyEnvelopeReader(size_t capacity = 1024);
    std::span<char> Acquire(size_t size);
    void Commit(size_t size);
    std::optional<TEnvelope> Pop();
    size_t Size() const;
    bool NeedMoreData() const {
        return !HasHeader || Size() < Header.Size;
    }

    // for testing purposes
    void Push(const char* p, size_t len);

private:
    void EnsureCapacity(size_t size);
    void CopyOut(char* buf, size_t size);
    void TryReadHeader();

    std::vector<char> Data;
    size_t Head = 0;
    size_t Tail = 0;
    size_t LastIndex = 0;

    bool HasHeader = false;
    THeader Header;
};

} // namespace NActors
} // namespace NNet