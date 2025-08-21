#pragma once

#include <vector>

/**
 * @file queue.hpp
 * @brief Memory-efficient unbounded queue implementation for actor message passing
 *
 * This file contains a queue implementation that avoids the
 * memory allocations/deallocations of std::queue (which uses std::deque).
 * The queue uses a single vector with power-of-two sizing and bitwise operations
 * for fast indexing, growing only when necessary.
 *
 * @section usage Usage Examples
 *
 * @subsection basic_usage Basic Queue Operations
 * @code
 * TUnboundedVectorQueue<int> queue;
 *
 * // Add elements
 * queue.Push(42);
 * queue.Push(100);
 *
 * // Process elements
 * while (!queue.Empty()) {
 *     int value = queue.Front();
 *     queue.Pop();
 *     processValue(value);
 * }
 * @endcode
 *
 * @subsection actor_usage Actor Message Queue
 * @code
 * TUnboundedVectorQueue<TEnvelope> messageQueue(64); // Start with 64 capacity
 *
 * // Producer (actor system)
 * messageQueue.Push(TEnvelope{sender, recipient, messageId, blob});
 *
 * // Consumer (actor)
 * if (!messageQueue.Empty()) {
 *     auto& envelope = messageQueue.Front();
 *     actor->Receive(envelope.MessageId, envelope.Blob, ctx);
 *     messageQueue.Pop();
 * }
 * @endcode
 */

namespace NNet {
namespace NActors {

/**
 * @brief Unbounded queue with automatic capacity growth
 * @tparam T Element type stored in the queue
 *
 * TUnboundedVectorQueue uses a single vector as circular buffer to avoid
 * the frequent chunk allocations/deallocations that std::queue (std::deque)
 * performs. Power-of-two sizing enables fast bitwise indexing operations.
 * The queue grows by doubling capacity when full, maintaining amortized O(1)
 * push operations while keeping memory usage predictable.
 *
 */
template<typename T>
struct TUnboundedVectorQueue {
    /**
     * @brief Construct queue with initial capacity
     * @param capacity Initial capacity (will be rounded up to power of two)
     */
    TUnboundedVectorQueue(size_t capacity = 16)
        : Data(RoundUpToPowerOfTwo(capacity))
        , LastIndex(Data.size() - 1)
    { }

    /**
     * @brief Add element to the back of the queue
     * @param item Element to add (moved into the queue)
     */
    void Push(T&& item) {
        EnsureCapacity();
        Data[Tail] = std::move(item);
        Tail = (Tail + 1) & LastIndex;
    }

    /**
     * @brief Get reference to the front element
     * @return Reference to the front element
     * @note Behavior is undefined if queue is empty
     */
    T& Front() {
        return Data[Head];
    }

    /**
     * @brief Remove the front element from the queue
     * @note Behavior is undefined if queue is empty
     */
    void Pop() {
        Head = (Head + 1) & LastIndex;
    }

    bool TryPop(T& item) {
        if (Empty()) {
            return false;
        }
        item = std::move(Data[Head]);
        Head = (Head + 1) & LastIndex;
        return true;
    }

    /**
     * @brief Get current number of elements in the queue
     * @return Number of elements currently stored
     */
    size_t Size() const {
        return (Data.size() + Tail - Head) & LastIndex;
    }

    /**
     * @brief Check if the queue is empty
     * @return true if queue contains no elements
     */
    bool Empty() const {
        return Head == Tail;
    }

private:
    static size_t RoundUpToPowerOfTwo(size_t value) {
        size_t power = 1;
        while (power < value) {
            power <<= 1;
        }
        return power;
    }

    void EnsureCapacity() {
        if (Size() == Data.size() - 1) [[unlikely]] {
            std::vector<T> newData(Data.size() * 2);
            auto size = Size();
            for (size_t i = 0; i < size; ++i) {
                newData[i] = std::move(Data[(Head + i) & LastIndex]);
            }
            Data = std::move(newData);
            Head = 0;
            Tail = size;
            LastIndex = Data.size() - 1;
        }
    }

    std::vector<T> Data;
    size_t Head = 0;
    size_t Tail = 0;
    size_t LastIndex = 0;
};

} // namespace NActors
} // namespace NNet