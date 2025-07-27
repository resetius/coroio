#pragma once

#include <stack>

namespace NNet {

/**
 * @class TArenaAllocator
 * @brief Arena allocator to preallocate memory for IOCP events.
 *
 * This class implements an arena allocator that preallocates memory in blocks (of size @c PoolSize)
 * to avoid individual heap allocations for each IOCP event structure. This is necessary for efficiency
 * when working with the IOCP API, which may require frequent allocation and deallocation of event objects.
 *
 * @tparam T The type of object to allocate (e.g. IOCP event structure).
 * @tparam PoolSize The number of objects in each preallocated pool (default is 1024).
 *
 * @code{.cpp}
 * // Example usage:
 * TArenaAllocator<TIO> allocator;
 * TIO* ioEvent = allocator.allocate();
 * // use ioEvent...
 * allocator.deallocate(ioEvent);
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
