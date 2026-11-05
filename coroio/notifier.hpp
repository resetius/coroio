#pragma once

#include <coroio/corochain.hpp>
#include <coroio/socket.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <type_traits>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace NNet {

#ifndef _WIN32

/**
 * @brief Interface of a thread-safe notification with a single asynchronous listener.
 *
 * Code that must not depend on the poller type holds an INotifier. The concrete
 * implementation is TNotifier.
 */
class INotifier {
public:
    virtual ~INotifier() = default;

    /**
     * @brief Signals the listener from any thread.
     *
     * Only the first call writes to the wakeup descriptor until the listener
     * consumes the signal. A notification sent before Wait() is retained.
     */
    virtual void Notify() = 0;

    /**
     * @brief Asynchronously waits for and consumes a notification.
     *
     * Call on the poller thread, with at most one outstanding wait. The caller
     * must check all available work after waking because notifications coalesce.
     * Destroying an unfinished future cancels its wait on the poller thread.
     *
     * @throws std::logic_error if another wait is already outstanding.
     */
    virtual TFuture<void> Wait() = 0;
};

namespace NDetail {

/// Creates the non-blocking wakeup descriptors: {read, write}. Both are the same eventfd on Linux.
std::array<int, 2> MakeNotifierFds();

/// Writes one wakeup signal. Returns normally if a signal is already queued.
void WriteNotifierSignal(int fd);

} // namespace NDetail

/**
 * @brief Thread-safe notification with a single asynchronous listener.
 *
 * Producers call Notify() from any thread after publishing work. Calls coalesce
 * until Wait() consumes the signal; a notification means "check for work", not
 * one work item. Wait() re-arms the notifier before returning to the listener.
 *
 * @code
 * TNotifier notifier(loop.Poller());
 *
 * // Producer thread: publish work, then signal the listener.
 * notifier.Notify();
 *
 * // Event-loop coroutine: wait, then drain available work.
 * co_await notifier.Wait();
 * @endcode
 *
 * Code that must not depend on the poller type holds an INotifier&.
 *
 * @tparam TPoller The poller type of the listener's event loop; deduced from the constructor argument.
 *
 * @note Linux uses eventfd; other POSIX platforms use a non-blocking pipe.
 *       Works with every POSIX poller: epoll, kqueue, poll, select, and uring.
 * @note The poller must outlive the notifier. Stop producers and destroy or
 *       complete all waits before destroying the notifier.
 */
template<typename TPoller>
class TNotifier final: public INotifier {
    using TFileHandle = typename TPoller::TFileHandle;

public:
    /// Creates a notifier bound to the listener's event-loop poller.
    explicit TNotifier(TPoller& poller)
        : TNotifier(poller, NDetail::MakeNotifierFds())
    { }

    ~TNotifier() override {
        assert(!Waiting_);
#ifndef __linux__
        ::close(WriteFd_);
#endif
    }

    TNotifier(const TNotifier&) = delete;
    TNotifier& operator=(const TNotifier&) = delete;

    void Notify() override {
        if (Pending_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            NDetail::WriteNotifierSignal(WriteFd_);
        } catch (...) {
            Pending_.store(false, std::memory_order_release);
            throw;
        }
    }

    TFuture<void> Wait() override {
        if (Waiting_) {
            throw std::logic_error("notifier already has a listener");
        }
        Waiting_ = true;
        struct TWaitGuard {
            TNotifier& Owner;
            bool Completed = false;
            ~TWaitGuard() {
                Owner.Waiting_ = false;
                if constexpr (!std::is_same_v<TFileHandle, NNet::TFileHandle>) {
                    // A cancelled driven read may have consumed the signal already;
                    // signal again so the notification is retained. Worst case is a spurious wakeup.
                    if (!Completed && Owner.Pending_.load(std::memory_order_acquire)) {
                        try {
                            NDetail::WriteNotifierSignal(Owner.WriteFd_);
                        } catch (...) {
                        }
                    }
                }
            }
        } guard{*this};

#ifdef __linux__
        uint64_t signal;
#else
        uint8_t signal;
#endif
        for (;;) {
            const auto size = co_await Handle_.ReadSome(&signal, sizeof(signal));
            if (size == static_cast<decltype(size)>(sizeof(signal))) {
                break;
            }
            if (size >= 0) {
                throw std::system_error(EIO, std::generic_category(), "notifier read");
            }
        }
        // Re-arm before the caller drains its work, so concurrent work can signal again.
        Pending_.exchange(false, std::memory_order_acq_rel);
        guard.Completed = true;
    }

private:
    TNotifier(TPoller& poller, std::array<int, 2> fds)
        : Handle_(fds[0], poller)
        , WriteFd_(fds[1])
    { }

    TFileHandle Handle_;
    int WriteFd_ = -1;
    std::atomic<bool> Pending_ = false;
    bool Waiting_ = false;
};

#endif // _WIN32

} // namespace NNet
