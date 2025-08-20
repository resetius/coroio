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

    if (HasHeader && Size() >= Header.Size) {
        TEnvelope envelope;
        envelope.Sender = Header.Sender;
        envelope.Recipient = Header.Recipient;
        envelope.MessageId = Header.MessageId;

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
    , CurrentChunk(std::make_unique<TChunk>(ChunkSize, this))
    , SealedChunks(1)
    , UsedChunks(2)
{ }

void TZeroCopyEnvelopeReaderV2::Rotate() {
    if (CurrentChunk->Size() == 0 && CurrentChunk->UseCount == 0) {
        CurrentChunk->Clear();
    } else {
        if (CurrentChunk->Size() == 0) {
            UsedChunks.PushBack(std::move(CurrentChunk));
        } else {
            SealedChunks.PushBack(std::move(CurrentChunk));
        }
        if (FreeChunks.empty()) {
            CurrentChunk = std::make_unique<TChunk>(ChunkSize, this);
        } else {
            CurrentChunk = std::move(FreeChunks.back());
            CurrentChunk->Clear();
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

void TZeroCopyEnvelopeReaderV2::TChunk::Clear() {
    Head = Tail = 0;
}

TZeroCopyEnvelopeReaderV2::TChunk::TChunk(size_t size, TZeroCopyEnvelopeReaderV2* parent)
    : Data(size)
    , Parent(parent)
{ }

std::span<char> TZeroCopyEnvelopeReaderV2::TChunk::Acquire(size_t size) {
    size = std::min(size, Data.size() - Tail);
    return {&Data[Tail], size};
}

std::span<char> TZeroCopyEnvelopeReaderV2::TChunk::TryAcquire(size_t size, size_t lowWatermark) {
    auto resultSize = std::min(size, Data.size() - Tail);
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
    assert (Data.size() - Head >= size);
    std::memcpy(buf, &Data[Head], size);
    Head = Head + size;
    assert (Head <= Tail);
    return Head == Tail;
}

bool TZeroCopyEnvelopeReaderV2::TChunk::CopyOut(char* buf, size_t size) {
    assert (Data.size() - Head >= size);
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
            FreeChunks.emplace_back(std::move(SealedChunks.PopFront()));
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
            FreeChunks.emplace_back(std::move(SealedChunks.PopFront()));
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
    ++chunk.UseCount;
    blob.Data = TBlob::TRawPtr(chunk.Data.data() + chunk.Head, TBlobDeleter{[&chunk](void* ptr) {
        if (--chunk.UseCount == 0) {
            auto* self = chunk.Parent;
            auto ref = self->UsedChunks.Erase(&chunk);
            if (ref) {
                self->FreeChunks.emplace_back(std::move(ref));
            }
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

    if (HasHeader && CurrentSize >= Header.Size) {
        TEnvelope envelope;
        envelope.Sender = Header.Sender;
        envelope.Recipient = Header.Recipient;
        envelope.MessageId = Header.MessageId;

        if (Header.Size > 0) {
            auto& blob = envelope.Blob;
            if (!SealedChunks.Empty() && SealedChunks.Front()->Size() >= Header.Size) {
                auto* front = SealedChunks.Front();
                blob = ExtractBlob(*front, Header.Size);
                if (front->Size() == 0) {
                    UsedChunks.PushBack(std::move(SealedChunks.PopFront()));
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
