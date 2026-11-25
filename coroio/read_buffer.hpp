#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>
#include <assert.h>

#include "arena.hpp"

namespace NNet {

/**
 * @brief Buffers of the ReadBuffer() reads of one poller.
 *
 * Set with TPollerBase::ConfigureReadBuffers() before the first ReadBuffer().
 * The memory, Count * BufferSize bytes, is allocated at the first ReadBuffer().
 */
struct TReadBufferConfig {
    uint32_t BufferSize = 16 * 1024; ///< Bytes of one buffer: a read gets at most this many.
    uint32_t Count = 1024; ///< Buffers of the poller, a power of two up to 32768.
    uint32_t PerSocketLimit = 8; ///< Leases a poller that reads ahead (TUring) queues on one descriptor.
};

namespace NDetail {
class TBufferPool;
} // namespace NDetail

/**
 * @brief Lease of a poller buffer that holds the bytes of one read.
 *
 * Returned by `co_await handle.ReadBuffer()`. Empty, i.e. Data() is empty, at
 * the end of the stream. The destructor gives the buffer back to the poller.
 * A lease must be destroyed on the poller's thread before the poller, and it
 * holds one of TReadBufferConfig::Count buffers: do not keep many of them.
 */
class TReadBuffer {
public:
    TReadBuffer() = default;
    TReadBuffer(TReadBuffer&& other) noexcept;
    TReadBuffer& operator=(TReadBuffer&& other) noexcept;
    TReadBuffer(const TReadBuffer&) = delete;
    TReadBuffer& operator=(const TReadBuffer&) = delete;
    ~TReadBuffer();

    /// The bytes not consumed yet.
    std::span<const char> Data() const {
        return {Ptr_ + Pos_, Size_ - Pos_};
    }

    /// Drops the first @p n bytes of Data().
    void Consume(size_t n) {
        assert(n <= Size_ - Pos_);
        Pos_ += static_cast<uint32_t>(n);
    }

private:
    friend class NDetail::TBufferPool;

    void Release();

    NDetail::TBufferPool* Pool_ = nullptr;
    char* Ptr_ = nullptr;
    uint32_t Size_ = 0;
    uint32_t Pos_ = 0;
    uint16_t Id_ = 0;
};

namespace NDetail {

/**
 * @brief The buffers of TReadBuffer leases of one poller: one TArena of Count slots.
 *
 * As with the zero-copy readers, a backend Acquire()s a buffer, the system reads
 * into it, and Commit() publishes the bytes read as a lease; the lease's destructor
 * Release()s the buffer. A poller whose system picks the buffers (TUring's buffer
 * ring, addressed by buffer id) sets an owner that takes them back instead of the
 * free list.
 */
class TBufferPool {
public:
    /// Takes back the buffers of a pool whose system picks them.
    struct IOwner {
        virtual void Release(uint16_t id) = 0;

    protected:
        ~IOwner() = default;
    };

    TBufferPool() = default;
    TBufferPool(const TBufferPool&) = delete;
    TBufferPool& operator=(const TBufferPool&) = delete;
    ~TBufferPool() {
        assert(Leased_ == 0); // a lease outlives its poller
    }

    /// Throws std::logic_error once the memory is allocated, std::invalid_argument for a bad config.
    void Configure(const TReadBufferConfig& config);

    const TReadBufferConfig& Config() const {
        return Config_;
    }

    /// Allocates the memory, once.
    void Allocate();

    /// Start of buffer @p id.
    char* Buffer(uint16_t id) const {
        return Arena_->Slot(id);
    }

    void SetOwner(IOwner* owner) {
        Owner_ = owner;
    }

    /**
     * @brief Acquires a free buffer for @p lease, which owns it with no bytes yet.
     *
     * @return The buffer to read into.
     * @throws std::system_error with std::errc::no_buffer_space if all buffers are leased.
     */
    std::span<char> Acquire(TReadBuffer& lease);

    /// Publishes the first @p size bytes read into the buffer of @p lease. With none, the buffer goes back.
    void Commit(TReadBuffer& lease, size_t size) {
        lease.Size_ = static_cast<uint32_t>(size);
        if (size == 0) {
            lease.Release();
        }
    }

    /// Publishes @p size bytes that the system read into buffer @p id as a lease.
    TReadBuffer Commit(uint16_t id, size_t size) {
        TReadBuffer lease;
        Lend(lease, id);
        lease.Size_ = static_cast<uint32_t>(size);
        return lease;
    }

    /// Gives buffer @p id of a lease back: to the owner, or to the free list.
    void Release(uint16_t id) {
        assert(std::this_thread::get_id() == Thread_); // a lease stays on its poller's thread
        assert(Leased_ > 0);
        Leased_--;
        if (Owner_) {
            Owner_->Release(id);
        } else {
            Arena_->Deallocate(Buffer(id));
        }
    }

private:
    void Lend(TReadBuffer& lease, uint16_t id) {
        assert(std::this_thread::get_id() == Thread_);
        Leased_++;
        lease.Pool_ = this;
        lease.Ptr_ = Buffer(id);
        lease.Size_ = lease.Pos_ = 0;
        lease.Id_ = id;
    }

    TReadBufferConfig Config_;
    std::optional<TArena> Arena_;
    IOwner* Owner_ = nullptr;
    size_t Leased_ = 0;
    std::thread::id Thread_;
};

} // namespace NDetail

inline TReadBuffer::TReadBuffer(TReadBuffer&& other) noexcept
    : Pool_(std::exchange(other.Pool_, nullptr))
    , Ptr_(std::exchange(other.Ptr_, nullptr))
    , Size_(std::exchange(other.Size_, 0))
    , Pos_(std::exchange(other.Pos_, 0))
    , Id_(other.Id_)
{ }

inline TReadBuffer& TReadBuffer::operator=(TReadBuffer&& other) noexcept {
    if (this != &other) {
        Release();
        Pool_ = std::exchange(other.Pool_, nullptr);
        Ptr_ = std::exchange(other.Ptr_, nullptr);
        Size_ = std::exchange(other.Size_, 0);
        Pos_ = std::exchange(other.Pos_, 0);
        Id_ = other.Id_;
    }
    return *this;
}

inline TReadBuffer::~TReadBuffer() {
    Release();
}

inline void TReadBuffer::Release() {
    if (Pool_) {
        std::exchange(Pool_, nullptr)->Release(Id_);
        Ptr_ = nullptr;
        Size_ = Pos_ = 0;
    }
}

} // namespace NNet
