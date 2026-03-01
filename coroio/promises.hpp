#pragma once
#include <coroutine>

namespace NNet {

struct TVoidPromise;

/**
 * @brief Fire-and-forget coroutine handle.
 *
 * Cannot be `co_await`-ed. Self-destructs on completion (`final_suspend →
 * suspend_never`). Unhandled exceptions are silently swallowed.
 *
 * Use for detached tasks that outlive their caller — e.g. per-connection
 * handlers spawned inside an accept loop.
 *
 * @code
 * TVoidTask handle_client(TSocket sock) {
 *     char buf[4096]; ssize_t n;
 *     while ((n = co_await sock.ReadSome(buf, sizeof(buf))) > 0)
 *         co_await sock.WriteSome(buf, n);
 * }
 * @endcode
 */
struct TVoidTask : std::coroutine_handle<TVoidPromise>
{
    using promise_type = TVoidPromise;
};

struct TVoidPromise
{
    TVoidTask get_return_object() { return { TVoidTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

struct TVoidSuspendedPromise;

/**
 * @brief Like TVoidTask but suspends at final_suspend instead of self-destructing.
 *
 * Used internally where the caller needs to manually destroy the coroutine
 * after it completes. Not for general use — prefer TVoidTask or TFuture<void>.
 */
struct TVoidSuspendedTask : std::coroutine_handle<TVoidSuspendedPromise>
{
    using promise_type = TVoidSuspendedPromise;
};

struct TVoidSuspendedPromise
{
    TVoidSuspendedTask get_return_object() { return { TVoidSuspendedTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

/**
 * @brief Awaitable that resumes immediately and yields this coroutine's own handle.
 *
 * `co_await Self{}` never actually suspends (`await_suspend` returns `false`),
 * but it captures the current coroutine handle via `await_suspend`. Used by
 * `Any()` to register the same coroutine as the continuation of multiple futures.
 *
 * @cond INTERNAL
 */
struct Self {
    bool await_ready() {
        return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
        H = h;
        return false;
    }

    auto await_resume() noexcept {
        return H;
    }

    std::coroutine_handle<> H;
};
/** @endcond */

} // namespace NNet
