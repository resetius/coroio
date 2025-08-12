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

        if (HasHeader && Buffer.size() >= Header.Size) {
            TEnvelope envelope;
            envelope.Sender = Header.Sender;
            envelope.Recipient = Header.Recipient;
            envelope.MessageId = Header.MessageId;

            if (Header.Size > 0) {
                auto& blob = envelope.Blob;
                blob.Size = Header.Size;
                blob.Type = TBlob::PointerType::Far;
                blob.Data = TBlob::TRawPtr(::operator new(blob.Size), [](void* ptr) {
                    ::operator delete(ptr);
                });
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

TZeroCopyEnvelopeReader::TZeroCopyEnvelopeReader(size_t capacity)
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
        CopyOut(reinterpret_cast<char*>(&Header), sizeof(THeader));
        HasHeader = true;
    }
}

std::optional<TEnvelope> TZeroCopyEnvelopeReader::Pop()
{
    TryReadHeader();

    if (HasHeader && Size() >= Header.Size) {
        TEnvelope envelope;
        envelope.Sender = Header.Sender;
        envelope.Recipient = Header.Recipient;
        envelope.MessageId = Header.MessageId;

        if (Header.Size > 0) {
            auto& blob = envelope.Blob;
            blob.Size = Header.Size;
            blob.Type = TBlob::PointerType::Far;
            blob.Data = TBlob::TRawPtr(::operator new(blob.Size), [](void* ptr) {
                ::operator delete(ptr);
            });
            CopyOut(static_cast<char*>(blob.Data.get()), Header.Size);
        }
        HasHeader = false;
        return std::make_optional(std::move(envelope));
    } else {
        return std::nullopt;
    }
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
    , CurrentChunk(std::make_unique<TChunk>(ChunkSize))
{ }

void TZeroCopyEnvelopeReaderV2::Rotate() {
    if (CurrentChunk->Size() > 0) [[likely]] {
        SealedChunks.Push(std::move(CurrentChunk));
    } else {
        FreeChunks.emplace_back(std::move(CurrentChunk));
    }
    if (FreeChunks.empty()) {
        CurrentChunk = std::make_unique<TChunk>(ChunkSize);
    } else {
        CurrentChunk = std::move(FreeChunks.back());
        CurrentChunk->Clear();
        FreeChunks.pop_back();
    }
}

std::span<char> TZeroCopyEnvelopeReaderV2::Acquire(size_t size) {
    auto buf = CurrentChunk->TryAcquire(size, LowWatermark);
    if (!buf.empty()) [[unlikely]] {
        return buf;
    }
    Rotate();
    return CurrentChunk->Acquire(size);
}

void TZeroCopyEnvelopeReaderV2::Commit(size_t size) {
    CurrentChunk->Commit(size);
    CurrentSize += size;
}

void TZeroCopyEnvelopeReaderV2::TChunk::Clear() {
    Head = Tail = 0;
}

TZeroCopyEnvelopeReaderV2::TChunk::TChunk(size_t size)
    : Data(size)
{ }

std::span<char> TZeroCopyEnvelopeReaderV2::TChunk::Acquire(size_t size) {
    size = std::min(size, Data.size() - Tail);
    return {&Data[Tail], size};
}

std::span<char> TZeroCopyEnvelopeReaderV2::TChunk::TryAcquire(size_t size, size_t lowWatermark) {
    size = std::min(size, Data.size() - Tail);
    return size < lowWatermark ? std::span<char>{} : Acquire(size);
}

void TZeroCopyEnvelopeReaderV2::TChunk::Commit(size_t size) {
    Tail = Tail + size;
}

bool TZeroCopyEnvelopeReaderV2::TChunk::CopyOut(char* buf, size_t size) {
    assert (Data.size() - Head >= size);
    std::memcpy(buf, &Data[Head], size);
    Head = Head + size;
    assert (Head <= Tail);
    return Head == Tail;
}

size_t TZeroCopyEnvelopeReaderV2::TChunk::Size() const {
    return Head - Tail;
}

size_t TZeroCopyEnvelopeReaderV2::CopyOut(char* buf, size_t psize) {
    size_t size = psize;
    while (!SealedChunks.Empty()) {
        auto&& chunk = SealedChunks.Front();
        auto sizeToCopy = std::min(size, chunk->Size());
        if (chunk->CopyOut(buf, sizeToCopy)) {
            FreeChunks.emplace_back(std::move(chunk));
            SealedChunks.Pop();
        }
        buf += sizeToCopy;
        size -= sizeToCopy;
    }

    if (size > 0) {
        auto sizeToCopy = std::min(size, CurrentChunk->Size());
        CurrentChunk->CopyOut(buf, sizeToCopy);
    }

    CurrentSize -= psize - size;
    return psize - size;
}

std::optional<TEnvelope> TZeroCopyEnvelopeReaderV2::Pop() {
    if (!HasHeader) {
        auto size = CopyOut(reinterpret_cast<char*>(&Header), sizeof(THeader));
        assert (size == 0 || size == sizeof(THeader));
        if (size == sizeof(THeader)) {
            HasHeader = true;
        }
    }

    if (HasHeader && CurrentSize >= Header.Size) {
        TEnvelope envelope;
        envelope.Sender = Header.Sender;
        envelope.Recipient = Header.Recipient;
        envelope.MessageId = Header.MessageId;

        if (Header.Size > 0) {
            auto& blob = envelope.Blob;
            blob.Size = Header.Size;
            blob.Type = TBlob::PointerType::Far;
            blob.Data = TBlob::TRawPtr(::operator new(blob.Size), [](void* ptr) {
                ::operator delete(ptr);
            });
            CopyOut(static_cast<char*>(blob.Data.get()), Header.Size);
        }
        HasHeader = false;
        return std::make_optional(std::move(envelope));
    } else {
        return std::nullopt;
    }
}

} // namespace NActors
} // namespace NNet
