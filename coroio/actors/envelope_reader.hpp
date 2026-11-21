#pragma once

#include "actor.hpp"
#include "queue.hpp"
#include "intrusive_list.hpp"

#include <atomic>
#include <deque>
#include <span>
#include <list>

namespace NNet {
namespace NActors {

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
    // False once the current frame is complete; Pop() still returns nothing if it is a filler
    bool NeedMoreData() const {
        return !HasHeader || Size() < HeaderPadding(Header) + Header.Size;
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

/**
 * Envelope reader that hands out payloads as blobs pointing into its chunks.
 * Blobs may be released on any thread and may outlive the reader. The last
 * blob of a drained chunk pushes it onto a lock-free return stack, which the
 * reader takes whole on its own thread. After the reader is destroyed the last
 * blob frees the chunk.
 * The reader itself is single-threaded.
 * Chunk addresses are congruent to stream offsets modulo `MaxPayloadAlignment`,
 * so a payload with `PayloadAlignment` is aligned in memory.
 */
class TZeroCopyEnvelopeReaderV2 {
public:
    TZeroCopyEnvelopeReaderV2(size_t chunkSize = 1024 * 1024, size_t lowWatermark = 64 * 1024);
    ~TZeroCopyEnvelopeReaderV2();

    std::span<char> Acquire(size_t size);
    void Commit(size_t size);
    std::optional<TEnvelope> Pop();
    size_t Size() const {
        return CurrentSize;
    }
    // False once the current frame is complete; Pop() still returns nothing if it is a filler
    bool NeedMoreData() const {
        return !HasHeader || Size() < HeaderPadding(Header) + Header.Size;
    }

    // for testing purposes
    void Push(const char* p, size_t len);

    // Drained chunks still held by live blobs
    int UsedChunksCount() const;

private:
    struct TChunk;

    // Shared with the chunks, so it outlives the reader while blobs are alive
    struct TReturnStack {
        // Returned chunks linked by NextReturned, or Closed once the reader is destroyed
        std::atomic<TChunk*> Head = nullptr;
    };
    static TChunk* const Closed;

    void Rotate();
    void Retire(std::unique_ptr<TChunk> chunk);
    void Reclaim();
    static void Return(TChunk* chunk);
    template<size_t size>
    bool CopyOut(char* buf);
    void CopyOut(char* buf, size_t size);
    TBlob ExtractBlob(TChunk& chunk, size_t size);

    struct TChunk : TIntrusiveListNode<TChunk> {
        TChunk(size_t size, uint64_t streamOffset, std::shared_ptr<TReturnStack> returned);

        std::span<char> TryAcquire(size_t size, size_t lowWatermark);
        std::span<char> Acquire(size_t size);
        void Commit(size_t size);
        template<size_t size>
        bool CopyOut(char* buf);
        bool CopyOut(char* buf, size_t size);
        size_t Size() const;
        // Empties the chunk; its next byte gets an address congruent to streamOffset
        void Clear(uint64_t streamOffset);

        // The chunk size plus room to shift the start
        std::vector<char> Data;
        size_t Head = 0;
        size_t Tail = 0;
        size_t End = 0;
        // Live blobs, plus one reference of the reader until the chunk is drained
        std::atomic<int> UseCount = 1;
        TChunk* NextReturned = nullptr;

        std::shared_ptr<TReturnStack> Returned;
    };

    const size_t ChunkSize;
    const size_t LowWatermark;
    size_t CurrentSize = 0;
    // Bytes committed since the start of the stream
    uint64_t StreamOffset = 0;
    // Takes the padding; a buffer on the stack would add a stack protector check to Pop
    char Skipped[MaxPayloadAlignment];
    THeader Header;
    bool HasHeader = false;
    std::shared_ptr<TReturnStack> Returned;
    std::unique_ptr<TChunk> CurrentChunk;
    TIntrusiveList<TChunk> SealedChunks;
    std::vector<std::unique_ptr<TChunk>> FreeChunks;
    // Drained chunks given to their blobs and not reclaimed yet
    int RetiredChunks = 0;
};

} // namespace NActors
} // namespace NNet