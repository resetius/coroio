#pragma once

#include <coroio/notifier.hpp>
#include <coroio/loop.hpp>

#include <cassert>
#include <thread>

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
 * @tparam TPoller Polling backend; any POSIX poller.
 *
 * @note Not available on Windows: it needs `TNotifier`.
 */
template<typename TPoller>
class TLoopThread {
public:
    TLoopThread()
        : StopNotifier_(Loop_.Poller())
        , Stopper_(WaitStop())
    { }

    TLoopThread(const TLoopThread&) = delete;
    TLoopThread& operator=(const TLoopThread&) = delete;

    /// Stops the loop and joins the thread.
    ~TLoopThread() {
        Stop();
        Join();
    }

    /// The poller of the loop. See the class description for which thread may use it.
    TPoller& Poller() {
        return Loop_.Poller();
    }

    /// Starts the loop thread. Call once, from the owner thread.
    void Start() {
        assert(!Thread_.joinable() && "TLoopThread is started once");
        Thread_ = std::thread([this] {
            Loop_.Loop();
        });
    }

    /**
     * @brief Asks the loop to exit. Any thread, any number of times.
     *
     * The loop exits after the step that sees the request, so a coroutine that never
     * returns to the loop delays it. A request made before `Start()` is kept: the loop
     * then makes one step and exits.
     */
    void Stop() {
        StopNotifier_.Notify();
    }

    /// Waits for the loop thread to exit. Owner thread only; does nothing if it is not running.
    void Join() {
        if (Thread_.joinable()) {
            Thread_.join();
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
    std::thread Thread_;
};

#endif // _WIN32

} // namespace NNet
