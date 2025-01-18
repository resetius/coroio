#pragma once

#include <coroutine>
#include <optional>
#include <variant>
#include <memory>
#include <functional>

#include "promises.hpp"
#include "poller.hpp"

namespace NNet {

template<typename T> struct TFinalAwaiter;

template<typename T> struct TFuture;

template<typename T>
struct TPromiseBase {
    std::suspend_never initial_suspend() { return {}; }
    TFinalAwaiter<T> final_suspend() noexcept;
    std::coroutine_handle<> Caller = std::noop_coroutine();
};

template<typename T>
struct TPromise: public TPromiseBase<T> {
    TFuture<T> get_return_object();

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
struct TFutureBase {
    TFutureBase() = default;
    TFutureBase(TPromise<T>& promise)
        : Coro(Coro.from_promise(promise))
    { }
    TFutureBase(TFutureBase&& other)
    {
        *this = std::move(other);
    }
    TFutureBase(const TFutureBase&) = delete;
    TFutureBase& operator=(const TFutureBase&) = delete;
    TFutureBase& operator=(TFutureBase&& other) {
        if (this != &other) {
            std::swap(Coro, other.Coro);
        }
        return *this;
    }

    ~TFutureBase() { if (Coro) { Coro.destroy(); } }

    bool await_ready() const {
        return Coro.promise().ErrorOr != std::nullopt;
    }

    bool done() const {
        return Coro.done();
    }

    std::coroutine_handle<> raw() {
        return Coro;
    }

    void await_suspend(std::coroutine_handle<> caller) {
        Coro.promise().Caller = caller;
    }

    using promise_type = TPromise<T>;

protected:
    std::coroutine_handle<TPromise<T>> Coro = nullptr;
};

template<> struct TFuture<void>;

template<typename T>
struct TFuture : public TFutureBase<T> {
    T await_resume() {
        auto& errorOr = *this->Coro.promise().ErrorOr;
        if (auto* res = std::get_if<T>(&errorOr)) {
            return std::move(*res);
        } else {
            std::rethrow_exception(std::get<std::exception_ptr>(errorOr));
        }
    }

    template<typename Func>
    auto Apply(Func func) -> TFuture<decltype(func(std::declval<T>()))> {
        auto prev = std::move(*this);
        auto ret = co_await prev;
        co_return func(ret);
    }

    TFuture<void> Ignore();
};

template<>
struct TPromise<void>: public TPromiseBase<void> {
    TFuture<void> get_return_object();

    void return_void() {
        ErrorOr = nullptr;
    }

    void unhandled_exception() {
        ErrorOr = std::current_exception();
    }

    std::optional<std::exception_ptr> ErrorOr;
};

template<>
struct TFuture<void> : public TFutureBase<void> {
    void await_resume() {
        auto& errorOr = *this->Coro.promise().ErrorOr;
        if (errorOr) {
            std::rethrow_exception(errorOr);
        }
    }

    template<typename Func>
    auto Accept(Func func) -> TFuture<void> {
        auto prev = std::move(*this);
        co_await prev;
        co_return func();
    }
};

template<typename T>
inline TFuture<void> TFuture<T>::Ignore() {
    auto prev = std::move(*this);
    co_await prev;
    co_return;
}

template<typename T>
struct TFinalAwaiter {
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise<T>> h) noexcept {
        return h.promise().Caller;
    }
    void await_resume() noexcept { }
};

inline TFuture<void> TPromise<void>::get_return_object() { return { TFuture<void>{*this} }; }
template<typename T>
TFuture<T> TPromise<T>::get_return_object() { return { TFuture<T>{*this} }; }


template<typename T>
TFinalAwaiter<T> TPromiseBase<T>::final_suspend() noexcept { return {}; }

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

template<typename T>
TFuture<T> Any(std::vector<TFuture<T>>&& futures) {
    std::vector<TFuture<T>> all = std::move(futures);

    auto it = std::find_if(all.begin(), all.end(), [](auto& f) { return f.done(); });
    if (it != all.end()) {
        co_return it->await_resume();
    }

    auto self = co_await Self();
    for (auto& f : all) {
        f.await_suspend(self);
    }
    co_await std::suspend_always();
    co_return std::find_if(all.begin(), all.end(), [](auto& f) { return f.done(); })->await_resume();
}

inline TFuture<void> Any(std::vector<TFuture<void>>&& futures) {
    std::vector<TFuture<void>> all = std::move(futures);

    if (std::find_if(all.begin(), all.end(), [](auto& f) { return f.done(); }) != all.end()) {
        co_return;
    }

    auto self = co_await Self();
    for (auto& f : all) {
        f.await_suspend(self);
    }
    co_await std::suspend_always();
    co_return;
}

} // namespace NNet
