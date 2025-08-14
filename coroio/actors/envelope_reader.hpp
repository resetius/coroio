#pragma once

#include "actor.hpp"
#include "queue.hpp"

#include <deque>
#include <span>
#include <list>

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
    size_t Size() const {
        return CurrentSize;
    }
    bool NeedMoreData() const {
        return !HasHeader || Size() < Header.Size;
    }

    // for testing purposes
    void Push(const char* p, size_t len);
    void PrintDebugInfo() const {
        std::cerr << "TZeroCopyEnvelopeReaderV2: CurrentSize = " << CurrentSize
                  << ", Chunks: " << SealedChunks.Size()
                  << ", FreeChunks: " << FreeChunks.size()
                  << ", CurrentChunk Size: " << CurrentChunk->Size()
                  << ", Head: " << CurrentChunk->Head
                  << ", Tail: " << CurrentChunk->Tail
                  << ", Ptr: " << static_cast<void*>(CurrentChunk->Data.data())
                  << "\n";
    }

    int UsedChunksCount() const {
        return UsedChunks.size();
    }

private:
    struct TChunk;

    void Rotate();
    void CopyOut(char* buf, size_t size);
    TBlob ExtractBlob(TChunk& chunk, size_t size);

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
        std::optional<std::list<std::unique_ptr<TChunk>>::iterator> Position;
    };

    const size_t ChunkSize;
    const size_t LowWatermark;
    size_t CurrentSize = 0;
    THeader Header;
    bool HasHeader = false;
    std::unique_ptr<TChunk> CurrentChunk;
    TUnboundedVectorQueue<std::unique_ptr<TChunk>> SealedChunks;
    std::vector<std::unique_ptr<TChunk>> FreeChunks;
    std::list<std::unique_ptr<TChunk>> UsedChunks;
};

} // namespace NActors
} // namespace NNet