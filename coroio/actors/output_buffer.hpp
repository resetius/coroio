#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace NNet {
namespace NActors {

/**
 * @brief Byte buffer of the outbound stream of `TNode`: grows without zero-filling.
 *
 * `std::vector<char>::resize` writes zeros into every byte it adds, and the writers overwrite
 * them right away. This buffer only owns memory: `Reserve` makes room, the caller tracks how many
 * bytes are in use and writes every byte before it reads it. The memory is not initialized, so a
 * serializer must not leave gaps in the range it was given.
 *
 * It grows geometrically and never shrinks. A block that is mostly full is grown by `realloc`,
 * which lets the allocator extend or remap it, and copies the whole old block only when it
 * cannot (at most twice the bytes in use). A block that holds mostly stale bytes is replaced by
 * a new one and only the bytes in use are copied.
 */
class TOutputBuffer {
public:
    /// The smallest block `Reserve` allocates: small frames do not cause a chain of tiny reallocations.
    static constexpr size_t MinCapacity = 4096;
    /// A block grows to this many times its size at least: the total copy stays proportional to the final size.
    static constexpr size_t GrowthFactor = 2;
    /// A block with less than 1/`StaleRatio` of its bytes in use is mostly stale (left from the
    /// previous use), so it is cheaper to copy the used bytes than to let `realloc` copy all of them.
    static constexpr size_t StaleRatio = 2;

    TOutputBuffer() = default;
    TOutputBuffer(const TOutputBuffer&) = delete;
    TOutputBuffer& operator=(const TOutputBuffer&) = delete;

    /// Takes the block; `other` is left empty.
    TOutputBuffer(TOutputBuffer&& other) noexcept
        : Ptr(std::move(other.Ptr))
        , Cap(std::exchange(other.Cap, 0))
    { }

    /// Frees the block of `this` and takes the block of `other`, which is left empty.
    TOutputBuffer& operator=(TOutputBuffer&& other) noexcept {
        if (this != &other) {
            Ptr = std::move(other.Ptr);
            Cap = std::exchange(other.Cap, 0);
        }
        return *this;
    }

    /// The start of the block, null while the capacity is 0. Valid until the next `Reserve` or `Swap`.
    char* Data() {
        return Ptr.get();
    }

    /// The size of the block in bytes.
    size_t Capacity() const {
        return Cap;
    }

    /// Makes room for `size` bytes and keeps the first `used` bytes (`used <= size`).
    /// Throws `std::bad_alloc` and leaves the buffer as it was.
    void Reserve(size_t used, size_t size) {
        if (size > Cap) [[unlikely]] {
            Grow(used, size);
        }
    }

    /// Exchanges the blocks of the two buffers.
    void Swap(TOutputBuffer& other) noexcept {
        Ptr.swap(other.Ptr);
        std::swap(Cap, other.Cap);
    }

private:
    struct TFree {
        void operator()(char* p) const {
            std::free(p);
        }
    };

    void Grow(size_t used, size_t size) {
        const size_t cap = std::max({size, Cap * GrowthFactor, MinCapacity});
        if (used * StaleRatio < Cap) {
            std::unique_ptr<char, TFree> block(static_cast<char*>(std::malloc(cap)));
            if (!block) {
                throw std::bad_alloc();
            }
            if (used > 0) {
                std::memcpy(block.get(), Ptr.get(), used);
            }
            Ptr = std::move(block);
        } else {
            // realloc(nullptr, n) is malloc(n); on failure the old block stays valid
            char* block = static_cast<char*>(std::realloc(Ptr.get(), cap));
            if (!block) {
                throw std::bad_alloc();
            }
            Ptr.release();
            Ptr.reset(block);
        }
        Cap = cap;
    }

    std::unique_ptr<char, TFree> Ptr;
    size_t Cap = 0;
};

/**
 * @brief Marks a range that `IOutputStream::Acquire` hands out as not written yet.
 *
 * In a build without `NDEBUG` it fills the range with `0xA5`, so a writer that leaves a gap in
 * it (padding, or the part of a payload that `SerializeTo` does not write) sends the marker,
 * which tests see, and not zeros or old bytes that hide the gap. With `NDEBUG` it does nothing.
 */
inline void PoisonAcquired([[maybe_unused]] std::span<char> range) {
#ifndef NDEBUG
    if (!range.empty()) {
        std::memset(range.data(), 0xA5, range.size());
    }
#endif
}

} // namespace NActors
} // namespace NNet
