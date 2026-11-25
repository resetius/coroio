#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <stack>
#include <vector>

namespace NNet {

/**
 * @class TArena
 * @brief Slots of one size, preallocated in pools, with a free list.
 *
 * The size of a slot and the number of slots of a pool are set at construction.
 * An arena of one pool (@p maxPools = 1) is one contiguous region, whose slots are
 * also addressed by index; Allocate() returns nullptr when all of them are in use.
 */
class TArena {
public:
    TArena(size_t slotSize, size_t alignment, size_t poolSize, size_t maxPools = std::numeric_limits<size_t>::max())
        : SlotSize_(slotSize)
        , Alignment_(alignment)
        , PoolSize_(poolSize)
        , MaxPools_(maxPools)
    {
        AllocatePool();
    }

    TArena(const TArena&) = delete;
    TArena& operator=(const TArena&) = delete;

    ~TArena() {
        for (auto block : Pools_) {
            ::operator delete(block, std::align_val_t(Alignment_));
        }
    }

    void* Allocate() {
        if (FreePages_.empty()) {
            if (Pools_.size() == MaxPools_) {
                return nullptr;
            }
            AllocatePool();
        }
        ++AllocatedObjects_;
        void* ret = FreePages_.top();
        FreePages_.pop();
        return ret;
    }

    void Deallocate(void* obj) {
        --AllocatedObjects_;
        FreePages_.push(obj);
    }

    int Count() const {
        return AllocatedObjects_;
    }

    /// Slot @p index of the first pool.
    char* Slot(size_t index) const {
        return Pools_.front() + index * SlotSize_;
    }

    /// Index of @p slot, a slot of the first pool.
    size_t Index(const void* slot) const {
        return static_cast<size_t>(static_cast<const char*>(slot) - Pools_.front()) / SlotSize_;
    }

private:
    void AllocatePool() {
        char* pool = static_cast<char*>(::operator new(PoolSize_ * SlotSize_, std::align_val_t(Alignment_)));
        Pools_.emplace_back(pool);
        for (size_t i = 0; i < PoolSize_; i++) {
            FreePages_.push(&pool[i * SlotSize_]);
        }
    }

    size_t SlotSize_;
    size_t Alignment_;
    size_t PoolSize_;
    size_t MaxPools_;
    std::vector<char*> Pools_;
    std::stack<void*, std::vector<void*>> FreePages_;
    int AllocatedObjects_ = 0;
};

/**
 * @class TArenaAllocator
 * @brief Arena allocator for objects of one type that are often created and destroyed.
 *
 * This class preallocates memory in pools of @c PoolSize objects and reuses the freed slots,
 * so that each object does not need its own heap allocation. It hands out raw storage:
 * construct the object in it with placement new and destroy the object before giving the
 * storage back.
 *
 * @tparam T The type of object to allocate.
 * @tparam PoolSize The number of objects in each preallocated pool (default is 1024).
 *
 * @code{.cpp}
 * // Example usage:
 * TArenaAllocator<TItem> allocator;
 * TItem* item = new (allocator.Allocate()) TItem();
 * // use item...
 * item->~TItem();
 * allocator.Deallocate(item);
 * @endcode
 */
template<typename T, size_t PoolSize = 1024>
class TArenaAllocator: public TArena {
public:
    TArenaAllocator()
        : TArena(sizeof(T), alignof(T), PoolSize)
    { }
};

} // namespace NNet
