namespace NNet {

/**
 * @brief Event loop that drives a poller backend.
 *
 * Each call to `Step()` runs one polling round: it calls `Poller_.Poll()` to
 * collect ready I/O events and timers, then `Poller_.WakeupReadyHandles()` to
 * resume the waiting coroutines.
 *
 * `Loop()` repeats `Step()` until `Stop()` is called. For manual control (e.g.
 * driving the loop until a specific `TFuture` completes), use `Step()` directly:
 *
 * @code
 * TLoop<TDefaultPoller> loop;
 * TFuture<void> task = my_coroutine(loop.Poller());
 * while (!task.done())
 *     loop.Step();
 * @endcode
 *
 * For server-style programs with detached `TVoidTask` coroutines:
 * @code
 * TLoop<TDefaultPoller> loop;
 * server(loop.Poller(), TAddress{"::", 8888});  // returns TVoidTask
 * loop.Loop();                                   // runs until Stop() is called
 * @endcode
 *
 * @tparam TPoller Polling backend: TEPoll, TKqueue, TIOCp, TUring, TPoll, TSelect, etc.
 */
template<typename TPoller>
class TLoop {
public:
    /**
     * @brief Runs the main loop until @ref Stop() is called.
     */
    void Loop() {
        while (Running_) {
            Step();
        }
    }
    /**
     * @brief Stops the loop.
     */
    void Stop() {
        Running_ = false;
    }
    /**
     * @brief Runs one event-loop iteration: polls for I/O/timer events then
     *        resumes all coroutines whose events fired.
     */
    void Step() {
        Poller_.Poll();
        Poller_.WakeupReadyHandles();
    }
    /**
     * @brief Provides access to the underlying poller instance.
     */
    TPoller& Poller() {
        return Poller_;
    }

private:
    TPoller Poller_;
    bool Running_ = true;
};

} // namespace NNet
