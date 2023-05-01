#pragma once

#include <system_error>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <queue>

#include <cstdint>
#include <cstdio>

#include <assert.h>

#include "poller.hpp"

namespace NNet {

struct TVoidPromise;

struct TSimpleTask : std::coroutine_handle<TVoidPromise>
{
    using promise_type = TVoidPromise;
};

struct TVoidPromise
{
    TSimpleTask get_return_object() { return { TSimpleTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

struct TTestPromise;

struct TTestTask : std::coroutine_handle<TTestPromise>
{
    using promise_type = TTestPromise;
};

struct TTestPromise
{
    TTestTask get_return_object() { return { TTestTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

struct TTestSuspendPromise;

struct TTestSuspendTask : std::coroutine_handle<TTestSuspendPromise>
{
    using promise_type = TTestSuspendPromise;
};

struct TTestSuspendPromise
{
    TTestSuspendTask get_return_object() { return { TTestSuspendTask::from_promise(*this) }; }
    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

template<typename TPoller>
class TLoop {
public:
    void Loop() {
        while (Running_) {
            Step();
        }
    }

    void Stop() {
        Running_ = false;
    }

    void Step() {
        Poller_.Poll();
        Poller_.WakeupReadyHandles();
    }

    TPoller& Poller() {
        return Poller_;
    }

private:
    TPoller Poller_;
    bool Running_ = true;
};

} /* namespace NNet */
