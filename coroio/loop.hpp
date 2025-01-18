namespace NNet {

/**
 * @class TLoop
 * @brief A generic event loop wrapper that uses TSelect as its poller.
 *
 * The event loop continuously calls @ref Step() until explicitly stopped via
 * @ref Stop().
 *
 * ### Example Usage
 * @code{.cpp}
 * #include <iostream>
 *
 * int main() {
 *     // Instantiate the event loop with TSelect
 *     TLoop<TSelect> loop;
 *
 *     // Run a single iteration of polling and wakeup
 *     loop.Step();
 *
 *     // Stop the loop
 *     loop.Stop();
 *
 *     // Alternatively, to run continuously:
 *     // loop.Loop(); // Will keep calling Step() until Stop() is called
 *
 *     return 0;
 * }
 * @endcode
 *
 * @tparam TPoller A type that provides polling and wakeup mechanisms (TSelect here).
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
     * @brief Performs a single iteration of polling and waking up ready handles.
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
