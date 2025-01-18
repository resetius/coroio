#pragma once
#include <coroutine>

namespace NNet {

struct TVoidPromise;

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
 * @class Self
 * @brief A minimal example of a coroutine "awaitable" object.
 *
 * This structure stores a coroutine handle and demonstrates how a coroutine
 * can be suspended and resumed.
 *
 * ### Example Usage
 * @code{.cpp}
 * // A simple coroutine that uses 'Self' as an awaitable:
 * std::coroutine_handle<> exampleCoroutine() {
 *     // co_await our custom awaitable
 *     auto handle = co_await Self{};
 *     // 'handle' is a std::coroutine_handle to this coroutine.
 *     // You can perform actions like handle.destroy(), handle.resume(), etc.
 *     ...
 * }
 * @endcode
 *
 * @note Internally, it captures the coroutine handle in its await_suspend step.
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
