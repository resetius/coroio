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

} // namespace NNet
