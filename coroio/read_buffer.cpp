#include "read_buffer.hpp"

#include <stdexcept>
#include <system_error>

namespace NNet::NDetail {

void TBufferPool::Configure(const TReadBufferConfig& config) {
    if (Arena_) {
        throw std::logic_error("read buffers are configured after their first use");
    }
    // Buffer ids are 16-bit, and a buffer ring takes a power of two of at most 32768 entries.
    if (config.BufferSize == 0 || config.PerSocketLimit == 0
        || config.Count == 0 || config.Count > 32768 || (config.Count & (config.Count - 1)))
    {
        throw std::invalid_argument("bad read buffer config");
    }
    Config_ = config;
}

void TBufferPool::Allocate() {
    if (!Arena_) {
        // One pool, so that a buffer has an id. Its pages are touched only when used.
        Arena_.emplace(Config_.BufferSize, 64, Config_.Count, 1);
        Thread_ = std::this_thread::get_id();
    }
}

std::span<char> TBufferPool::Acquire(TReadBuffer& lease) {
    assert(!Owner_); // the owner's system picks the buffers
    Allocate();
    void* buffer = Arena_->Allocate();
    if (!buffer) {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "read buffers are all leased");
    }
    lease = TReadBuffer{};
    Lend(lease, static_cast<uint16_t>(Arena_->Index(buffer)));
    return {lease.Ptr_, Config_.BufferSize};
}

} // namespace NNet::NDetail
