#pragma once

#include <chrono>
#include <iostream>
#include <unordered_set>
#include <vector>
#include <map>
#include <queue>
#include <assert.h>

#include "base.hpp"

#ifdef Yield
#undef Yield
#endif

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

namespace NNet {

/**
 * @brief Backend-independent base for I/O pollers.
 *
 * Manages three concerns shared by all poller backends:
 * - **Timers** — `AddTimer` / `RemoveTimer` / `ProcessTimers`
 * - **I/O event registration** — `AddRead`, `AddWrite`, `AddRemoteHup`, `RemoveEvent`
 * - **Coroutine scheduling** — `Sleep`, `Yield`, `Wakeup`, `WakeupReadyHandles`
 *
 * Concrete backends (TEPoll, TKqueue, …) inherit from this class and implement
 * `Poll()` to fill `ReadyEvents_` using the OS-specific mechanism, then call
 * `WakeupReadyHandles()` (provided here) to resume waiting coroutines.
 */
class TPollerBase {
public:
    /// Default constructor.
    TPollerBase() = default;

    /// Copying is disabled.
    TPollerBase(const TPollerBase& ) = delete;
    TPollerBase& operator=(const TPollerBase& ) = delete;

    /**
     * @brief Schedules a timer.
     *
     * Inserts a timer event with the given deadline and coroutine handle into the internal timer queue.
     *
     * @param deadline The time at which the timer should fire.
     * @param h        The handle of the coroutine to resume when the timer expires.
     * @return A unique timer ID.
     */
    unsigned AddTimer(TTime deadline, THandle h) {
        Timers_.emplace(TTimer{deadline, TimerId_, h});
        return TimerId_++;
    }

    /**
     * @brief Removes or cancels a timer.
     *
     * Checks if the specified timer (by its ID) has fired based on the deadline; if not, inserts an
     * empty timer to force removal.
     *
     * @param timerId  The timer ID to remove.
     * @param deadline The associated deadline.
     * @return True if the timer had already fired; false otherwise.
     */
    bool RemoveTimer(unsigned timerId, TTime deadline) {
        bool fired = timerId == LastFiredTimer_;
        if (!fired) {
            Timers_.emplace(TTimer{deadline, timerId, {}}); // insert empty timer before existing
        }
        return fired;
    }

    /**
     * @brief Registers a read interest on a file descriptor.
     *
     * `h` will be resumed by `WakeupReadyHandles()` on the next loop iteration
     * after the fd becomes readable.
     *
     * @param fd File descriptor to watch.
     * @param h  Coroutine to resume when data is available.
     */
    void AddRead(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::READ, h});
    }
    /**
     * @brief Registers a write interest on a file descriptor.
     *
     * `h` will be resumed by `WakeupReadyHandles()` on the next loop iteration
     * after the fd becomes writable.
     *
     * @param fd File descriptor to watch.
     * @param h  Coroutine to resume when ready for writing.
     */
    void AddWrite(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::WRITE, h});
    }
    /**
     * @brief Registers a remote hang-up (RHUP) event.
     *
     * @param fd The file descriptor.
     * @param h  The coroutine handle to resume when the remote end closes the connection.
     */
    void AddRemoteHup(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::RHUP, h});
    }
    /**
     * @brief Removes registered events for a specific file descriptor.
     *
     * This method signals that events for a descriptor should be deregistered.
     *
     * @param fd The file descriptor.
     */
    void RemoveEvent(int fd) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::READ|TEvent::WRITE|TEvent::RHUP, {}});
        if (!ReadyEvents_.empty()) {
            RemovedFdsCurrentLoop_.insert(fd);
        }
    }

    void RemoveReadyHandle(THandle h) {
        if (!ReadyEvents_.empty()) {
            RemovedHandlesCurrentLoop_.insert(h.address());
        }
    }
    /**
     * @brief Suspends execution until the specified time.
     *
     * Returns an awaitable object that registers a timer and suspends the current coroutine until the deadline.
     *
     * @param until The time until which to sleep.
     * @return An awaitable sleep object.
     */
    auto Sleep(TTime until) {
        struct TAwaitableSleep {
            TAwaitableSleep(TPollerBase* poller, TTime n)
                : poller(poller)
                , n(n)
            { }
            ~TAwaitableSleep() {
                if (poller) {
                    poller->RemoveTimer(timerId, n);
                }
            }

            TAwaitableSleep(TAwaitableSleep&& other)
                : poller(other.poller)
                , n(other.n)
            {
                other.poller = nullptr;
            }

            TAwaitableSleep(const TAwaitableSleep&) = delete;
            TAwaitableSleep& operator=(const TAwaitableSleep&) = delete;

            bool await_ready() {
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) {
                timerId = poller->AddTimer(n, h);
            }

            void await_resume() { poller = nullptr; }

            TPollerBase* poller;
            TTime n;
            unsigned timerId = 0;
        };

        return TAwaitableSleep{this,until};
    }
    /**
     * @brief Overload of Sleep() accepting a duration.
     *
     * @tparam Rep   The representation type.
     * @tparam Period The period type.
     * @param duration The duration to sleep.
     * @return An awaitable sleep object.
     */
    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        return Sleep(TClock::now() + duration);
    }
    /**
     * @brief Suspends the coroutine until the next event-loop iteration.
     *
     * Equivalent to `Sleep(TTime{})`, which fires immediately on the next
     * `ProcessTimers()` call. Use when you want to give other coroutines a
     * chance to run before continuing.
     *
     * @return An awaitable that resumes on the next loop tick.
     */
    auto Yield() {
        return Sleep(TTime{});
    }

    /**
     * @brief Wakes up a coroutine waiting on an event.
     *
     * This method resumes the coroutine associated with the given TEvent (@p change),
     * then checks whether that coroutine has added a new wait for the same file descriptor.
     *
     * @param change The event change containing the file descriptor and coroutine handle.
     */
    void Wakeup(TEvent&& change) {
       /**
        * Wakes up the coroutine associated with the given TEvent `change`, then checks
        * whether that coroutine has added a new wait on the same file descriptor (Fd).
        * We first store the current size of the `Changes_` vector and call `change.Handle.resume()`.
        *
        * If after resuming, the coroutine does NOT add (or match) a wait entry for `change.Fd`,
        * we assume that this descriptor is not in use yet and remove it from the poller
        * by resetting `change.Handle` and appending `change` to `Changes_`.
        *
        * In most cases, `Changes_` grows by exactly one entry after resuming a coroutine
        * (it suspends again on a descriptor), but there are scenarios where it can grow
        * by more than one. For example, one coroutine might launch another coroutine
        * without waiting; that second coroutine suspends on ReadSome/WriteSome, returns
        * control to the first coroutine, which then also suspends on ReadSome/WriteSome.
        * In that chain of calls, multiple new waits can be added before returning here,
        * so `Changes_` can increase by 2 (or potentially more) in a single wake-up cycle.
        */
        auto index = Changes_.size();
        change.Handle.resume();
        if (change.Fd >= 0) {
            bool matched = false;
            for (; index < Changes_.size() && !matched; index++) {
                matched = Changes_[index].Match(change);
            }
            if (!matched) {
                change.Handle = {};
                Changes_.emplace_back(std::move(change));
            }
        }
    }
    /**
     * @brief Wakes up all coroutines waiting on ready events.
     *
     * Iterates over the list of ready events and calls @ref Wakeup() on each.
     */
    void WakeupReadyHandles() {
        for (auto&& ev : ReadyEvents_) {
            if (!ev.Handle) { continue; }
            if (!RemovedFdsCurrentLoop_.empty()) [[unlikely]] {
                if (ev.Fd >= 0 && RemovedFdsCurrentLoop_.count(ev.Fd)) { continue; }
            }
            if (!RemovedHandlesCurrentLoop_.empty()) [[unlikely]] {
                if (RemovedHandlesCurrentLoop_.count(ev.Handle.address())) {
                    RemovedHandlesCurrentLoop_.erase(ev.Handle.address());
                    continue;
                }
            }
            Wakeup(std::move(ev));
        }
        RemovedFdsCurrentLoop_.clear();
        RemovedHandlesCurrentLoop_.clear();
    }
    /**
     * @brief Sets the maximum polling duration.
     *
     * @param maxDuration The maximum duration for a poll cycle.
     */
    void SetMaxDuration(std::chrono::milliseconds maxDuration) {
        MaxDuration_ = maxDuration;
        MaxDurationTs_ = GetMaxDuration(MaxDuration_);
    }
    /// Returns the number of scheduled timers.
    auto TimersSize() const {
        return Timers_.size();
    }

protected:
    /**
     * @brief Computes the poll timeout based on scheduled timers.
     *
     * @return A timespec representing the timeout.
     */
    timespec GetTimeout() const {
        return Timers_.empty()
            ? MaxDurationTs_
            : Timers_.top().Deadline == TTime{}
                ? timespec {0, 0}
                : GetTimespec(TClock::now(), Timers_.top().Deadline, MaxDuration_);
    }
    /**
     * @brief Computes a timespec from a duration.
     *
     * @param duration The maximum duration in milliseconds.
     * @return A timespec representing the duration.
     */
    static constexpr timespec GetMaxDuration(std::chrono::milliseconds duration) {
        auto p1 = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto p2 = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - p1);
        timespec ts;
        ts.tv_sec = p1.count();
        ts.tv_nsec = p2.count();
        return ts;
    }
    /// Clears the lists of ready events and pending changes.
    void Reset() {
        ReadyEvents_.clear();
        Changes_.clear();
        MaxFd_ = 0;
        RemovedFdsCurrentLoop_.clear();
        RemovedHandlesCurrentLoop_.clear();
    }
    /**
     * @brief Processes scheduled timers.
     *
     * Resumes waiting coroutines for timers that have expired. The method tracks whether a timer was
     * fired by comparing its ID to LastFiredTimer_.
     */
    void ProcessTimers() {
        auto now = TClock::now();
        bool first = true;
        unsigned prevId = 0;

        while (!Timers_.empty()&&Timers_.top().Deadline <= now) {
            TTimer timer = Timers_.top(); Timers_.pop();

            if ((first || prevId != timer.Id) && timer.Handle) { // skip removed timers
                LastFiredTimer_ = timer.Id;
                timer.Handle.resume();
            }

            first = false;
            prevId = timer.Id;
        }

        LastTimersProcessTime_ = now;
    }

    int MaxFd_ = 0; ///< Highest file descriptor in use.
    std::vector<TEvent> Changes_; ///< Pending changes (registered events).
    std::vector<TEvent> ReadyEvents_; ///< Events ready to wake up their coroutines.
    std::unordered_set<int>   RemovedFdsCurrentLoop_;     ///< Fds cancelled mid-loop (fd-based backends).
    std::unordered_set<void*> RemovedHandlesCurrentLoop_; ///< Handles cancelled mid-loop (uring/IOCP).
    unsigned TimerId_ = 0; ///< Counter for generating unique timer IDs.
    std::priority_queue<TTimer> Timers_; ///< Priority queue for scheduled timers.
    TTime LastTimersProcessTime_; ///< Last time timers were processed.
    unsigned LastFiredTimer_ = (unsigned)(-1); ///< ID of the last fired timer.
    std::chrono::milliseconds MaxDuration_ = std::chrono::milliseconds(100); ///< Maximum poll duration.
    timespec MaxDurationTs_ = GetMaxDuration(MaxDuration_); ///< Max duration represented as timespec.
};

} // namespace NNet
