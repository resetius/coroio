#pragma once

#include <coroutine>
#include <optional>
#include <variant>
#include <memory>
#include <functional>

namespace NNet {

template<typename T> struct TFinalAwaiter;

template<typename T> struct TValueTask;

template<typename T>
struct TValuePromiseBase {
    std::suspend_never initial_suspend() { return {}; }
    TFinalAwaiter<T> final_suspend() noexcept;
    std::coroutine_handle<> Caller = std::noop_coroutine();
};

template<typename T>
struct TValuePromise: public TValuePromiseBase<T> {
    TValueTask<T> get_return_object();

    void return_value(const T& t) {
        ErrorOr = t;
    }

    void return_value(T&& t) {
        ErrorOr = std::move(t);
    }

    void unhandled_exception() {
        ErrorOr = std::current_exception();
    }

    std::optional<std::variant<T, std::exception_ptr>> ErrorOr;
};

template<typename T>
struct TValueTaskBase {
    TValueTaskBase() = default;
    TValueTaskBase(TValuePromise<T>& promise)
        : Coro(Coro.from_promise(promise))
    { }
    TValueTaskBase(TValueTaskBase&& other)
    {
        *this = std::move(other);
    }
    TValueTaskBase(const TValueTaskBase&) = delete;
    TValueTaskBase& operator=(const TValueTaskBase&) = delete;
    TValueTaskBase& operator=(TValueTaskBase&& other) {
        if (this != &other) {
            std::swap(Coro, other.Coro);
        }
        return *this;
    }

    ~TValueTaskBase() { if (Coro) { Coro.destroy(); } }

    bool await_ready() const {
        return !!Coro.promise().ErrorOr;
    }

    bool done() const {
        return Coro.done();
    }

    void* address() const {
        return Coro.address();
    }

    std::coroutine_handle<> raw() {
        return Coro;
    }

    void await_suspend(std::coroutine_handle<> caller) {
        Coro.promise().Caller = caller;
    }

    using promise_type = TValuePromise<T>;

protected:
    std::coroutine_handle<TValuePromise<T>> Coro = nullptr;
};

template<> struct TValueTask<void>;

template<typename T>
struct TValueTask : public TValueTaskBase<T> {
    T await_resume() {
        auto& errorOr = *this->Coro.promise().ErrorOr;
        if (auto* res = std::get_if<T>(&errorOr)) {
            return std::move(*res);
        } else {
            std::rethrow_exception(std::get<std::exception_ptr>(errorOr));
        }
    }

    template<typename Func>
    auto Apply(Func func) -> TValueTask<decltype(func(std::declval<T>()))> {
        auto prev = std::move(*this);
        auto ret = co_await prev;
        co_return func(ret);
    }

    TValueTask<void> Ignore();
};

template<>
struct TValuePromise<void>: public TValuePromiseBase<void> {
    TValueTask<void> get_return_object();

    void return_void() {
        ErrorOr = nullptr;
    }

    void unhandled_exception() {
        ErrorOr = std::current_exception();
    }

    std::optional<std::exception_ptr> ErrorOr;
};

template<>
struct TValueTask<void> : public TValueTaskBase<void> {
    void await_resume() {
        auto& errorOr = *this->Coro.promise().ErrorOr;
        if (errorOr) {
            std::rethrow_exception(errorOr);
        }
    }

    template<typename Func>
    auto Accept(Func func) -> TValueTask<void> {
        auto prev = std::move(*this);
        co_await prev;
        co_return func();
    }
};

template<typename T>
inline TValueTask<void> TValueTask<T>::Ignore() {
    auto prev = std::move(*this);
    co_await prev;
    co_return;
}

template<typename T>
struct TFinalAwaiter {
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<TValuePromise<T>> h) noexcept {
        return h.promise().Caller;
    }
    void await_resume() noexcept { }
};

inline TValueTask<void> TValuePromise<void>::get_return_object() { return { TValueTask<void>(*this) }; }
template<typename T>
TValueTask<T> TValuePromise<T>::get_return_object() { return { TValueTask<T>(*this) }; }


template<typename T>
TFinalAwaiter<T> TValuePromiseBase<T>::final_suspend() noexcept { return {}; }

template<typename T>
using TFuture = TValueTask<T>;

template<typename T>
TFuture<std::vector<T>> All(std::vector<TFuture<T>>&& futures) {
    auto waiting = std::move(futures);
    std::vector<T> ret; ret.reserve(waiting.size());
    for (auto& f : waiting) {
        ret.emplace_back(std::move(co_await f));
    }
    co_return ret;
}

inline TFuture<void> All(std::vector<TFuture<void>>&& futures) {
    auto waiting = std::move(futures);
    for (auto& f : waiting) {
        co_await f;
    }
    co_return;
}

inline TFuture<void> Any(std::vector<TFuture<void>>&& futures) {
    std::vector<TFuture<void>> waiting; waiting.reserve(futures.size());
    struct TAwaitable {
        bool await_ready() {
            return done;
        }

        void await_suspend(std::coroutine_handle<> h) {
            this->h = h;
        }

        void await_resume() { }

        bool done = false;
        std::coroutine_handle<> h = nullptr;
    };
    bool done = false;
    for (auto&& f : futures) {
        done = done || f.done();
    }
    TAwaitable awaitable{done};
    for (auto&& f : futures) {
        waiting.emplace_back(std::move(f.Accept([&](){
            awaitable.h.resume();
        })));
    }
    co_await awaitable;
    co_return;
}

} // namespace NNet
