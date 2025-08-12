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

class TZeroCopyEnvelopeReaderV2 {
public:
    TZeroCopyEnvelopeReaderV2(size_t chunkSize = 1024 * 1024, size_t lowWatermark = 64 * 1024);

    std::span<char> Acquire(size_t size);
    void Commit(size_t size);
    std::optional<TEnvelope> Pop();
    size_t Size() const;

    // for testing purposes
    void Push(const char* p, size_t len);

private:
    void Rotate();
    size_t CopyOut(char* buf, size_t size);

    struct TChunk {
        TChunk(size_t size);

        std::span<char> TryAcquire(size_t size, size_t lowWatermark);
        std::span<char> Acquire(size_t size);
        void Commit(size_t size);
        bool CopyOut(char* buf, size_t size);
        size_t Size() const;
        void Clear();

        std::vector<char> Data;
        size_t Head = 0;
        size_t Tail = 0;
        int UseCount = 0;
    };

    const size_t ChunkSize;
    const size_t LowWatermark;
    size_t CurrentSize = 0;
    THeader Header;
    bool HasHeader = false;
    std::unique_ptr<TChunk> CurrentChunk;
    TUnboundedVectorQueue<std::unique_ptr<TChunk>> SealedChunks;
    std::vector<std::unique_ptr<TChunk>> FreeChunks;
};

} // namespace NActors
} // namespace NNet