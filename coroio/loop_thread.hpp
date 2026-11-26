#pragma once

#include <coroio/notifier.hpp>
#include <coroio/loop.hpp>

#include <cassert>
#include <exception>
#include <functional>
#include <thread>
#include <utility>

namespace NNet {

#ifndef _WIN32

/**
 * @brief A thread that runs its own event loop.
 *
 * Owns a `TLoop<TPoller>` and a thread that runs it between `Start()` and
 * `Stop()`. The poller is single-threaded, so it has one owner at a time:
 *
 * - until `Start()` and after `Join()` — the thread that owns the TLoopThread;
 * - in between — the loop thread only.
 *
 * Create the sockets, notifiers and coroutines of this poller before `Start()`
 * and destroy them after `Join()`. While the loop runs, other threads reach its
 * coroutines only through thread-safe objects: `INotifier`, `TMpscQueue`.
 *
 * @code
 * TLoopThread<TDefaultPoller> io;
 * TNotifier notifier(io.Poller());
 * TFuture<void> consumer = Consume(notifier, queue); // runs up to its first wait
 * io.Start();
 * ...
 * io.Stop();
 * io.Join();
 * // consumer is destroyed here, before io
 * @endcode
 *
 * `Stop()` does not destroy coroutines: they stay suspended where they were and
 * are destroyed by their owners after `Join()`. A stopped loop cannot be started again.
 *
 * A poller closes a descriptor at its next poll, and after `Join()` there is none:
 * sockets and notifiers destroyed after `Join()` stay open until the TLoopThread is
 * destroyed, and only then does a peer see the end of its stream.
 *
 * An exception that ends the loop (a poller throws `std::system_error` when its
 * system call fails) does not terminate the process: `Join()` rethrows it. An owner
 * that must learn of it at once, not at `Join()`, passes a failure hook to `Start()`.
 *
 * @tparam TPoller Polling backend; any POSIX poller.
 *
 * @note Not available on Windows: it needs `TNotifier`.
 */
template<typename TPoller>
class TLoopThread {
public:
    /// Called on the loop thread with the exception that ended the loop, see `Start()`.
    using TFailureHook = std::function<void(std::exception_ptr)>;

    TLoopThread()
        : StopNotifier_(Loop_.Poller())
        , Stopper_(WaitStop())
    { }

    TLoopThread(const TLoopThread&) = delete;
    TLoopThread& operator=(const TLoopThread&) = delete;

    /// Stops the loop and joins the thread. An exception of the loop that `Join()` has
    /// not rethrown is dropped. No other thread may call `Stop()` during or after this.
    ~TLoopThread() {
        Stop();
        if (Thread_.joinable()) {
            Thread_.join();
        }
    }

    /// The poller of the loop. See the class description for which thread may use it.
    TPoller& Poller() {
        return Loop_.Poller();
    }

    /**
     * @brief Starts the loop thread. Call once, from the owner thread.
     *
     * @param onFailure Optional. If the loop ends with an exception, it is called with that
     *        exception on the loop thread, before the thread exits; `Join()` still rethrows
     *        the exception. A loop that is stopped does not call it. It must not throw: an
     *        exception that leaves it terminates the process. Use it when the owner must act
     *        at once, for example to end a process that cannot work without this loop.
     */
    void Start(TFailureHook onFailure = {}) {
        assert(!Thread_.joinable() && "TLoopThread is started once");
        Thread_ = std::thread([this, onFailure = std::move(onFailure)] {
            try {
                Loop_.Loop();
            } catch (...) {
                // Read by Join() after join(), which orders it after this write
                Error_ = std::current_exception();
                if (onFailure) {
                    onFailure(Error_);
                }
            }
        });
    }

    /**
     * @brief Asks the loop to exit. Any thread, any number of times, while the
     *        TLoopThread is alive: not concurrently with its destructor.
     *
     * The loop exits after the step that sees the request, so a coroutine that never
     * returns to the loop delays it. A request made before `Start()` is kept: the loop
     * then makes one step and exits.
     */
    void Stop() {
        StopNotifier_.Notify();
    }

    /**
     * @brief Waits for the loop thread to exit. Owner thread only; does nothing if it is not running.
     *
     * @throws The exception that ended the loop, if there was one; only the first call after it.
     */
    void Join() {
        if (Thread_.joinable()) {
            Thread_.join();
        }
        if (Error_) {
            std::rethrow_exception(std::exchange(Error_, nullptr));
        }
    }

private:
    // TLoop::Stop() is not thread-safe, so the request comes through the notifier
    // and the loop thread stops its own loop.
    TFuture<void> WaitStop() {
        co_await StopNotifier_.Wait();
        Loop_.Stop();
    }

    TLoop<TPoller> Loop_;
    TNotifier<TPoller> StopNotifier_;
    TFuture<void> Stopper_;
    std::exception_ptr Error_;
    std::thread Thread_;
};

#endif // _WIN32

} // namespace NNet
