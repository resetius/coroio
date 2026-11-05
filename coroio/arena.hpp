#pragma once

#include <stack>
#include <vector>

namespace NNet {

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
class TArenaAllocator {
public:
    TArenaAllocator() {
        AllocatePool();
    }

    ~TArenaAllocator() {
        for (auto block : Pools_) {
            ::operator delete(block, std::align_val_t(alignof(T)));
        }
    }

    void* Allocate() {
        if (FreePages_.empty()) {
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

private:
    void AllocatePool() {
        char* pool = static_cast<char*>(::operator new(PoolSize * sizeof(T), std::align_val_t(alignof(T))));
        Pools_.emplace_back(pool);
        for (size_t i = 0; i < PoolSize; i++) {
            FreePages_.push(&pool[i * sizeof(T)]);
        }
    }

    std::vector<char*> Pools_;
    std::stack<void*, std::vector<void*>> FreePages_;
    int AllocatedObjects_ = 0;
};

} // namespace NNet
