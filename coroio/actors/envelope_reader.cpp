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

} // namespace NActors
} // namespace NNet