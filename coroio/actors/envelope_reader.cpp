#include "envelope_reader.hpp"

namespace NNet {
namespace NActors {

void TEnvelopeReader::Push(const char* buf, size_t size) {
    Buffer.insert(Buffer.end(), buf, buf + size);
    Process();
}

void TEnvelopeReader::Process() {
    while (Buffer.size() >= sizeof(THeader)) {
        if (!HasHeader) {
            std::copy(Buffer.begin(), Buffer.begin() + sizeof(THeader), reinterpret_cast<char*>(&Header));
            Buffer.erase(Buffer.begin(), Buffer.begin() + sizeof(THeader));
            HasHeader = true;
        }

        const size_t padding = HeaderPadding(Header);
        if (HasHeader && Buffer.size() >= padding + Header.Size) {
            Buffer.erase(Buffer.begin(), Buffer.begin() + padding);
            if (padding > 0 && HeaderMessageId(Header) == FillerMessageId) [[unlikely]] {
                Buffer.erase(Buffer.begin(), Buffer.begin() + Header.Size);
                HasHeader = false;
                continue;
            }
            TEnvelope envelope;
            envelope.Sender = Header.Sender;
            envelope.Recipient = Header.Recipient;
            envelope.MessageId = HeaderMessageId(Header);

            if (Header.Size > 0) {
                auto& blob = envelope.Blob;
                blob.Size = Header.Size;
                blob.Type = TBlob::PointerType::Far;
                blob.Data = TBlob::TRawPtr(::operator new(blob.Size), TBlobDeleter{[](void* ptr) {
                    ::operator delete(ptr);
                }});
                std::copy(Buffer.begin(), Buffer.begin() + Header.Size, static_cast<char*>(blob.Data.get()));
                Buffer.erase(Buffer.begin(), Buffer.begin() + Header.Size);
            }
            Messages.push(std::move(envelope));
            HasHeader = false;
        }
    }
}

std::optional<TEnvelope> TEnvelopeReader::Pop() {
    if (Messages.empty()) {
        return std::nullopt;
    }

    auto envelope = std::move(Messages.front());
    Messages.pop();
    return std::make_optional(std::move(envelope));
}

TZeroCopyEnvelopeReader::TZeroCopyEnvelopeReader(size_t capacity, size_t unused)
    : Data(capacity)
    , LastIndex(Data.size() - 1)
{ }

size_t TZeroCopyEnvelopeReader::Size() const {
    return (Data.size() + Tail - Head) & LastIndex;
}

void TZeroCopyEnvelopeReader::EnsureCapacity(size_t size) {
    while (size + Size() >= Data.size()) [[unlikely]] {
        std::vector<char> newData(Data.size() * 2);
        auto size = Size();
        if (size > 0) {
            if (Head + size <= Data.size()) {
                std::memcpy(newData.data(), &Data[Head], size);
            } else {
                auto first = Data.size() - Head;
                std::memcpy(newData.data(), &Data[Head], first);
                std::memcpy(newData.data() + first, &Data[0], size - first);
            }
        }
        Data = std::move(newData);
        Head = 0;
        Tail = size;
        LastIndex = Data.size() - 1;
    }
}

std::span<char> TZeroCopyEnvelopeReader::Acquire(size_t size)
{
    EnsureCapacity(size);
    size = std::min(size, Data.size() - Size());
    auto first = std::min(size, Data.size() - Tail);
    if (first) {
        return {&Data[Tail], first};
    } else {
        return {&Data[0], size};
    }
}

void TZeroCopyEnvelopeReader::Commit(size_t size)
{
    Tail = (Tail + size) & LastIndex;
    TryReadHeader();
}

void TZeroCopyEnvelopeReader::TryReadHeader() {
    if (!HasHeader && Size() >= sizeof(THeader)) {
        CopyOut<sizeof(THeader)>(reinterpret_cast<char*>(&Header));
        HasHeader = true;
    }
}

std::optional<TEnvelope> TZeroCopyEnvelopeReader::Pop()
{
    TryReadHeader();

    const size_t padding = HeaderPadding(Header);
    if (HasHeader && Size() >= padding + Header.Size) {
        // A branch keeps the Head update off the path of unpadded frames
        if (padding > 0) {
            Head = (Head + padding) & LastIndex;
            // The filler always has padding, so unpadded frames skip this check
            if (HeaderMessageId(Header) == FillerMessageId) [[unlikely]] {
                Head = (Head + Header.Size) & LastIndex;
                HasHeader = false;
                return Pop();
            }
        }
        TEnvelope envelope;
        envelope.Sender = Header.Sender;
        envelope.Recipient = Header.Recipient;
        envelope.MessageId = HeaderMessageId(Header);

        if (Header.Size > 0) {
            auto& blob = envelope.Blob;
            blob.Size = Header.Size;
            blob.Type = TBlob::PointerType::Far;
            blob.Data = TBlob::TRawPtr(::operator new(blob.Size), TBlobDeleter{[](void* ptr) {
                ::operator delete(ptr);
            }});
            CopyOut(static_cast<char*>(blob.Data.get()), Header.Size);
        }
        HasHeader = false;
        return std::make_optional(std::move(envelope));
    } else {
        return std::nullopt;
    }
}

template<size_t size>
void TZeroCopyEnvelopeReader::CopyOut(char* buf)
{
    if (Data.size() - Head >= size) {
        std::memcpy(buf, &Data[Head], size);
    } else {
        auto first = Data.size() - Head;
        std::memcpy(buf, &Data[Head], first);
        std::memcpy(buf + first, &Data[0], size - first);
    }

    Head = (Head + size) & LastIndex;
}

void TZeroCopyEnvelopeReader::CopyOut(char* buf, size_t size) {
    if (Data.size() - Head >= size) {
        std::memcpy(buf, &Data[Head], size);
    } else {
        auto first = Data.size() - Head;
        std::memcpy(buf, &Data[Head], first);
        std::memcpy(buf + first, &Data[0], size - first);
    }

    Head = (Head + size) & LastIndex;
}

void TZeroCopyEnvelopeReader::Push(const char* p, size_t len)
{
    while (len != 0) {
        auto buf = Acquire(len);
        memcpy(buf.data(), p, buf.size());
        Commit(buf.size());
        len -= buf.size();
        p += buf.size();
    }
}

TZeroCopyEnvelopeReaderV2::TZeroCopyEnvelopeReaderV2(size_t chunkSize, size_t lowWatermark)
    : ChunkSize(chunkSize)
    , LowWatermark(lowWatermark)
    , Returned(std::make_shared<TReturnStack>())
    , CurrentChunk(std::make_unique<TChunk>(ChunkSize, StreamOffset, Returned))
    , SealedChunks(1)
{ }

TZeroCopyEnvelopeReaderV2::TChunk* const TZeroCopyEnvelopeReaderV2::Closed =
    reinterpret_cast<TZeroCopyEnvelopeReaderV2::TChunk*>(uintptr_t{1});

TZeroCopyEnvelopeReaderV2::~TZeroCopyEnvelopeReaderV2() {
    auto* chunk = Returned->Head.exchange(Closed, std::memory_order_acq_rel);
    while (chunk) {
        auto* next = chunk->NextReturned;
        delete chunk;
        chunk = next;
    }
    // From now on the last blob of a retired chunk frees it
    Retire(std::move(CurrentChunk));
    while (!SealedChunks.Empty()) {
        Retire(SealedChunks.PopFront());
    }
}

void TZeroCopyEnvelopeReaderV2::Retire(std::unique_ptr<TChunk> chunk) {
    // Drops the reader reference, the chunk then belongs to its live blobs
    if (chunk->UseCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        FreeChunks.emplace_back(std::move(chunk));
    } else {
        chunk.release();
        ++RetiredChunks;
    }
}

// Pushes onto the return stack. No ABA: nothing pops single chunks, the reader takes the
// whole stack with exchange, and a push only needs the current head to be its next.
void TZeroCopyEnvelopeReaderV2::Return(TChunk* chunk) {
    auto& head = chunk->Returned->Head;
    auto* next = head.load(std::memory_order_relaxed);
    do {
        if (next == Closed) {
            delete chunk;
            return;
        }
        chunk->NextReturned = next;
    } while (!head.compare_exchange_weak(next, chunk, std::memory_order_release, std::memory_order_relaxed));
}

void TZeroCopyEnvelopeReaderV2::Reclaim() {
    auto* chunk = Returned->Head.exchange(nullptr, std::memory_order_acquire);
    while (chunk) {
        auto* next = chunk->NextReturned;
        FreeChunks.emplace_back(chunk);
        --RetiredChunks;
        chunk = next;
    }
}

int TZeroCopyEnvelopeReaderV2::UsedChunksCount() const {
    // Only the reader takes chunks off the stack, so walking it here is safe
    int returned = 0;
    for (auto* chunk = Returned->Head.load(std::memory_order_acquire); chunk; chunk = chunk->NextReturned) {
        ++returned;
    }
    return RetiredChunks - returned;
}

void TZeroCopyEnvelopeReaderV2::Rotate() {
    // Only the reader adds blobs, so a count of 1 cannot grow concurrently
    if (CurrentChunk->Size() == 0 && CurrentChunk->UseCount.load(std::memory_order_acquire) == 1) {
        CurrentChunk->Clear(StreamOffset);
    } else {
        if (CurrentChunk->Size() == 0) {
            Retire(std::move(CurrentChunk));
        } else {
            SealedChunks.PushBack(std::move(CurrentChunk));
        }
        if (FreeChunks.empty()) {
            Reclaim();
        }
        if (FreeChunks.empty()) {
            CurrentChunk = std::make_unique<TChunk>(ChunkSize, StreamOffset, Returned);
        } else {
            CurrentChunk = std::move(FreeChunks.back());
            CurrentChunk->Clear(StreamOffset);
            CurrentChunk->UseCount.store(1, std::memory_order_relaxed);
            FreeChunks.pop_back();
        }
    }
}

std::span<char> TZeroCopyEnvelopeReaderV2::Acquire(size_t size) {
    auto buf = CurrentChunk->TryAcquire(size, LowWatermark);
    if (!buf.empty()) [[likely]] {
        return buf;
    }
    Rotate();
    return CurrentChunk->Acquire(size);
}

void TZeroCopyEnvelopeReaderV2::Commit(size_t size) {
    CurrentChunk->Commit(size);
    CurrentSize += size;
    StreamOffset += size;
}

void TZeroCopyEnvelopeReaderV2::Push(const char* p, size_t len)
{
    while (len != 0) {
        auto buf = Acquire(len);
        memcpy(buf.data(), p, buf.size());
        Commit(buf.size());
        len -= buf.size();
        p += buf.size();
    }
}

void TZeroCopyEnvelopeReaderV2::TChunk::Clear(uint64_t streamOffset) {
    constexpr size_t mask = MaxPayloadAlignment - 1;
    Head = Tail = (streamOffset - reinterpret_cast<uintptr_t>(Data.data())) & mask;
    End = Head + Data.size() - mask;
}

TZeroCopyEnvelopeReaderV2::TChunk::TChunk(size_t size, uint64_t streamOffset, std::shared_ptr<TReturnStack> returned)
    : Data(size + MaxPayloadAlignment - 1)
    , Returned(std::move(returned))
{
    Clear(streamOffset);
}

std::span<char> TZeroCopyEnvelopeReaderV2::TChunk::Acquire(size_t size) {
    size = std::min(size, End - Tail);
    return {&Data[Tail], size};
}

std::span<char> TZeroCopyEnvelopeReaderV2::TChunk::TryAcquire(size_t size, size_t lowWatermark) {
    auto resultSize = std::min(size, End - Tail);
    if (resultSize == size) {
        return Acquire(size);
    }
    return resultSize < lowWatermark ? std::span<char>{} : Acquire(size);
}

void TZeroCopyEnvelopeReaderV2::TChunk::Commit(size_t size) {
    Tail = Tail + size;
}

template<size_t size>
bool TZeroCopyEnvelopeReaderV2::TChunk::CopyOut(char* buf) {
    assert (End - Head >= size);
    std::memcpy(buf, &Data[Head], size);
    Head = Head + size;
    assert (Head <= Tail);
    return Head == Tail;
}

bool TZeroCopyEnvelopeReaderV2::TChunk::CopyOut(char* buf, size_t size) {
    assert (End - Head >= size);
    std::memcpy(buf, &Data[Head], size);
    Head = Head + size;
    assert (Head <= Tail);
    return Head == Tail;
}

size_t TZeroCopyEnvelopeReaderV2::TChunk::Size() const {
    return Tail - Head;
}

template<size_t size>
bool TZeroCopyEnvelopeReaderV2::CopyOut(char* buf) {
    if (!SealedChunks.Empty()) {
        auto* chunk = SealedChunks.Front();
        if (chunk->Size() < size) [[unlikely]] {
            return false;
        }
        if (chunk->CopyOut<size>(buf)) {
            Retire(SealedChunks.PopFront());
        }
    } else {
        if (CurrentChunk->Size() < size) [[unlikely]] {
            return false;
        }
        CurrentChunk->CopyOut<size>(buf);
    }
    CurrentSize -= size;
    return true;
}

void TZeroCopyEnvelopeReaderV2::CopyOut(char* buf, size_t size) {
    CurrentSize -= size;

    while (!SealedChunks.Empty() && size > 0) {
        auto* chunk = SealedChunks.Front();
        auto sizeToCopy = std::min(size, chunk->Size());
        assert(sizeToCopy > 0);
        if (chunk->CopyOut(buf, sizeToCopy)) {
            Retire(SealedChunks.PopFront());
        }
        buf += sizeToCopy;
        size -= sizeToCopy;
    }

    if (size > 0) {
        CurrentChunk->CopyOut(buf, size);
    }
}

TBlob TZeroCopyEnvelopeReaderV2::ExtractBlob(TChunk& chunk, size_t size) {
    TBlob blob;
    blob.Size = size;
    blob.Type = TBlob::PointerType::Far;
    // The reader holds a reference, so the count cannot drop to zero here
    chunk.UseCount.fetch_add(1, std::memory_order_relaxed);
    blob.Data = TBlob::TRawPtr(chunk.Data.data() + chunk.Head, TBlobDeleter{[&chunk](void* ptr) {
        if (chunk.UseCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Return(&chunk);
        }
    }});
    chunk.Head += size;
    CurrentSize -= size;
    return blob;
}

std::optional<TEnvelope> TZeroCopyEnvelopeReaderV2::Pop() {
    if (!HasHeader && CurrentSize >= sizeof(THeader)) {
        if (!CopyOut<sizeof(THeader)>(reinterpret_cast<char*>(&Header))) [[unlikely]] {
            // Header was split across chunks, fallback to copying out the header
            CopyOut(reinterpret_cast<char*>(&Header), sizeof(THeader));
        }
        HasHeader = true;
    }

    const size_t padding = HeaderPadding(Header);
    if (HasHeader && CurrentSize >= padding + Header.Size) {
        if (padding > 0) {
            CopyOut(Skipped, padding);
            // The filler always has padding, so unpadded frames skip this check
            if (HeaderMessageId(Header) == FillerMessageId) [[unlikely]] {
                for (size_t left = Header.Size; left > 0; ) {
                    const size_t size = std::min(left, sizeof(Skipped));
                    CopyOut(Skipped, size);
                    left -= size;
                }
                HasHeader = false;
                return Pop();
            }
        }
        TEnvelope envelope;
        envelope.Sender = Header.Sender;
        envelope.Recipient = Header.Recipient;
        envelope.MessageId = HeaderMessageId(Header);

        if (Header.Size > 0) {
            auto& blob = envelope.Blob;
            if (!SealedChunks.Empty() && SealedChunks.Front()->Size() >= Header.Size) {
                auto* front = SealedChunks.Front();
                blob = ExtractBlob(*front, Header.Size);
                if (front->Size() == 0) {
                    Retire(SealedChunks.PopFront());
                }
            } else if (SealedChunks.Empty() && CurrentChunk->Size() >= Header.Size) {
                blob = ExtractBlob(*CurrentChunk, Header.Size);
            } else {
                // Discontinuous data, we need to copy it out
                blob.Size = Header.Size;
                blob.Type = TBlob::PointerType::Far;
                blob.Data = TBlob::TRawPtr(::operator new(blob.Size), TBlobDeleter{[](void* ptr) {
                    ::operator delete(ptr);
                }});
                CopyOut(static_cast<char*>(blob.Data.get()), Header.Size);
            }
        }
        HasHeader = false;
        return std::make_optional(std::move(envelope));
    } else {
        return std::nullopt;
    }
}

} // namespace NActors
} // namespace NNet
