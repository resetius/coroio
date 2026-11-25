#pragma once

#include <atomic>
#include <memory>

namespace NNet {

/**
 * @brief Link of a TMpscQueue item. The item type derives from it:
 *        `struct TItem : TMpscQueueNode<TItem> { ... };`
 */
template<typename T>
struct TMpscQueueNode {
    T* Next = nullptr;
};

/**
 * @brief Unbounded lock-free queue with many producers and one consumer.
 *
 * Carries items between threads without allocating: the link is a part of the
 * item (`TMpscQueueNode`). `Push` may be called from any thread, `Pop` from one
 * thread at a time. Items of one producer are popped in the order it pushed them.
 *
 * Producers push onto a shared lock-free stack (R. K. Treiber, "Systems Programming:
 * Coping with Parallelism", IBM Research Report RJ 5118, 1986). The consumer never
 * pops one item from it: it takes the whole stack with one exchange and reverses it,
 * so there is no ABA problem and an item is never touched by a producer after `Push`.
 *
 * The queue does not wake the consumer by itself. `Push` returns true when the shared
 * stack was empty, and only then may the consumer be asleep. With an `INotifier` of
 * the consumer's poller:
 *
 * @code
 * // Any producer thread
 * if (queue.Push(std::move(item))) {
 *     notifier.Notify();
 * }
 *
 * // Consumer coroutine on the notifier's poller
 * while (true) {
 *     co_await notifier.Wait();
 *     while (auto item = queue.Pop()) {
 *         Process(*item);
 *     }
 * }
 * @endcode
 *
 * The consumer must pop until `Pop` returns nullptr before it waits again: a push
 * onto a non-empty stack does not notify. Between two pops it may suspend on anything
 * else, and under a steady load it should yield to its loop from time to time.
 *
 * @tparam T Item type derived from `TMpscQueueNode<T>`.
 *
 * @note Stop the producers before destroying the queue. The destructor deletes the
 *       items that were not popped.
 */
template<typename T>
class TMpscQueue {
public:
    TMpscQueue() = default;

    TMpscQueue(const TMpscQueue&) = delete;
    TMpscQueue& operator=(const TMpscQueue&) = delete;

    ~TMpscQueue() {
        while (Pop()) {
        }
    }

    /**
     * @brief Appends an item. Any thread.
     *
     * @return true if the shared stack was empty, so the consumer may be asleep
     *         and must be notified.
     */
    bool Push(std::unique_ptr<T> item) noexcept {
        T* node = item.release();
        // The value is only stored into node->Next, and the CAS checks that it is still the head.
        T* head = Head_.load(std::memory_order_relaxed);
        do {
            node->Next = head;
            // Release publishes the item and its link to the acquire exchange in Pop.
            // The head is changed only by CAS and exchange, so that exchange is also
            // ordered after the pushes of the items below (release sequence).
        } while (!Head_.compare_exchange_weak(head, node, std::memory_order_release, std::memory_order_relaxed));
        return head == nullptr;
    }

    /**
     * @brief Removes the oldest item, or returns nullptr if there is none. Consumer thread only.
     */
    std::unique_ptr<T> Pop() noexcept {
        if (!Ready_) {
            // An idle consumer must not take the cache line of the head from the producers.
            // A push that this load misses found the stack empty and notifies.
            if (!Head_.load(std::memory_order_relaxed)) {
                return nullptr;
            }
            // Acquire pairs with the release CAS in Push.
            T* node = Head_.exchange(nullptr, std::memory_order_acquire);
            while (node) {
                T* next = node->Next;
                node->Next = Ready_;
                Ready_ = node;
                node = next;
            }
        }
        T* node = Ready_;
        if (!node) {
            return nullptr;
        }
        Ready_ = node->Next;
        node->Next = nullptr;
        return std::unique_ptr<T>(node);
    }

private:
    // Newest first; shared with the producers
    std::atomic<T*> Head_{nullptr};
    // Oldest first; taken from Head_, used only by the consumer
    T* Ready_ = nullptr;
};

} // namespace NNet
