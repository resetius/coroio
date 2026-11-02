#pragma once

#include <coroio/corochain.hpp>

#include <atomic>

namespace NNet {

#ifndef _WIN32

/**
 * @brief Thread-safe notification with a single asynchronous listener.
 *
 * Producers call Notify() from any thread after publishing work. Calls coalesce
 * until Wait() consumes the signal; a notification means "check for work", not
 * one work item. Wait() re-arms the notifier before returning to the listener.
 *
 * @code
 * TNotifier notifier(poller);
 *
 * // Producer thread: publish work, then signal the listener.
 * notifier.Notify();
 *
 * // Event-loop coroutine: wait, then drain available work.
 * co_await notifier.Wait();
 * @endcode
 *
 * @note Linux uses eventfd; other POSIX platforms use a non-blocking pipe.
 *       Supports readiness pollers: epoll, kqueue, poll, and select.
 * @note The poller must outlive the notifier. Stop producers and destroy or
 *       complete all waits before destroying the notifier.
 */
class TNotifier {
public:
    /// Creates a notifier bound to the listener's event-loop poller.
    explicit TNotifier(TPollerBase& poller);
    ~TNotifier();

    TNotifier(const TNotifier&) = delete;
    TNotifier& operator=(const TNotifier&) = delete;

    /**
     * @brief Signals the listener from any thread.
     *
     * Only the first call writes to the wakeup descriptor until the listener
     * consumes the signal. A notification sent before Wait() is retained.
     */
    void Notify();

    /**
     * @brief Asynchronously waits for and consumes a notification.
     *
     * Call on the poller thread, with at most one outstanding wait. The caller
     * must check all available work after waking because notifications coalesce.
     * Destroying an unfinished future cancels its wait on the poller thread.
     *
     * @throws std::logic_error if another wait is already outstanding.
     */
    TFuture<void> Wait();

private:
    bool TryRead();

    TPollerBase& Poller_;
    int ReadFd_ = -1;
    int WriteFd_ = -1;
    std::atomic<bool> Pending_ = false;
    bool Waiting_ = false;
};

#endif // _WIN32

} // namespace NNet
