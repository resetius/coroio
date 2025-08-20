#pragma once

#include "actor.hpp"
#include "queue.hpp"
#include "intrusive_list.hpp"

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
    TEnvelopeReader(size_t unused1 = 0, size_t unused2 = 0) { }
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
    TZeroCopyEnvelopeReader(size_t capacity = 1024, size_t unused = 0);
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
    template<size_t size>
    void CopyOut(char* buf);
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

    int UsedChunksCount() const {
        return UsedChunks.Size();
    }

private:
    struct TChunk;

    void Rotate();
    template<size_t size>
    bool CopyOut(char* buf);
    void CopyOut(char* buf, size_t size);
    TBlob ExtractBlob(TChunk& chunk, size_t size);

    struct TChunk : TIntrusiveListNode<TChunk> {
        TChunk(size_t size, TZeroCopyEnvelopeReaderV2* parent);

        std::span<char> TryAcquire(size_t size, size_t lowWatermark);
        std::span<char> Acquire(size_t size);
        void Commit(size_t size);
        template<size_t size>
        bool CopyOut(char* buf);
        bool CopyOut(char* buf, size_t size);
        size_t Size() const;
        void Clear();

        std::vector<char> Data;
        size_t Head = 0;
        size_t Tail = 0;
        int UseCount = 0;

        TZeroCopyEnvelopeReaderV2* Parent = nullptr;
    };

    const size_t ChunkSize;
    const size_t LowWatermark;
    size_t CurrentSize = 0;
    THeader Header;
    bool HasHeader = false;
    std::unique_ptr<TChunk> CurrentChunk;
    TIntrusiveList<TChunk> SealedChunks;
    std::vector<std::unique_ptr<TChunk>> FreeChunks;
    TIntrusiveList<TChunk> UsedChunks;
};

} // namespace NActors
} // namespace NNet