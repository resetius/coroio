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

} // namespace NActors
} // namespace NNet