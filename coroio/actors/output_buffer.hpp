#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
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
 * It grows geometrically, and copies only the bytes in use. Large blocks are grown by `realloc`,
 * which lets the allocator remap pages instead of copying them. The buffer never shrinks by
 * itself, so a burst of the same size finds the room it needs; `ReleaseIfSparse` returns a large
 * block that a smaller burst did not use.
 */
class TOutputBuffer {
public:
    /// The smallest block `Reserve` allocates.
    static constexpr size_t MinCapacity = 4096;
    /// `ReleaseIfSparse` keeps blocks up to this size whatever their use.
    static constexpr size_t RetainedCapacity = 1 << 20;
    /// `ReleaseIfSparse` frees a larger block after this many sparse uses in a row.
    static constexpr unsigned SparseUsesToRelease = 8;

    char* Data() {
        return Ptr.get();
    }

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

    /// Call it after each use of the block, `used` being the bytes it carried. Frees the block if
    /// it is larger than `RetainedCapacity` and the last `SparseUsesToRelease` uses took less than
    /// a quarter of it each: memory is not pinned at the peak of bursts that are gone, while
    /// bursts of a varying size keep the block.
    void ReleaseIfSparse(size_t used) {
        if (Cap <= RetainedCapacity || used >= Cap / 4) {
            SparseUses = 0;
            return;
        }
        if (++SparseUses >= SparseUsesToRelease) {
            Ptr.reset();
            Cap = 0;
            SparseUses = 0;
        }
    }

    void Swap(TOutputBuffer& other) noexcept {
        Ptr.swap(other.Ptr);
        std::swap(Cap, other.Cap);
        std::swap(SparseUses, other.SparseUses);
    }

private:
    struct TFree {
        void operator()(char* p) const {
            std::free(p);
        }
    };

    void Grow(size_t used, size_t size) {
        const size_t cap = std::max({size, Cap * 2, MinCapacity});
        if (used * 2 < Cap) {
            // Most of the old block is stale bytes: copying only the used ones is cheaper than realloc
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
        SparseUses = 0;
    }

    std::unique_ptr<char, TFree> Ptr;
    size_t Cap = 0;
    unsigned SparseUses = 0;
};

} // namespace NActors
} // namespace NNet
