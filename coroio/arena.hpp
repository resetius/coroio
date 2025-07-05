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
            ::operator delete(block);
        }
    }

    T* allocate() {
        if (FreePages_.empty()) {
            AllocatePool();
        }
        ++AllocatedObjects_;
        T* ret = FreePages_.top();
        FreePages_.pop();
        return ret;
    }

    void deallocate(T* obj) {
        --AllocatedObjects_;
        FreePages_.push(obj);
    }

    int count() const {
        return AllocatedObjects_;
    }

private:
    void AllocatePool() {
        T* pool = static_cast<T*>(::operator new(PoolSize * sizeof(T)));
        Pools_.emplace_back(pool);
        for (size_t i = 0; i < PoolSize; i++) {
            FreePages_.push(&pool[i]);
        }
    }

    std::vector<T*> Pools_;
    std::stack<T*> FreePages_;
    int AllocatedObjects_ = 0;
};

} // namespace NNet
