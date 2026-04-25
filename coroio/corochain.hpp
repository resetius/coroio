#pragma once

/**
 * @file corochain.hpp
 * @brief Implementation of a promise/future system for coroutines.
 *
 * This file defines the promise and future types used to manage coroutine
 * execution. It provides mechanisms to retrieve coroutine results (or exceptions)
 * and to coordinate multiple asynchronous operations.
 *
 * ## Example Usage
 *
 * @code
 * #include <coroio/corochain.hpp>
 * #include <vector>
 * #include <iostream>
 *
 * using namespace NNet;
 *
 * // A coroutine that returns an integer.
 * TFuture<int> getInt() {
 *     co_return 42;
 * }
 *
 * // A coroutine that returns void.
 * TFuture<void> doSomething() {
 *     // Perform some work...
 *     co_return;
 * }
 *
 * // A coroutine that waits for all futures to complete.
 * TFuture<void> testAll() {
 *     std::vector<TFuture<int>> futures;
 *     futures.push_back(getInt());
 *     futures.push_back(getInt());
 *
 *     auto results = co_await All(std::move(futures));
 *     // Process results...
 *     for (auto res : results) {
 *         std::cout << "Result: " << res << std::endl;
 *     }
 *     co_return;
 * }
 *
 * // A coroutine that waits for any future to complete.
 * TFuture<int> testAny() {
 *     std::vector<TFuture<int>> futures;
 *     futures.push_back(getInt());
 *     futures.push_back(getInt());
 *
 *     int result = co_await Any(std::move(futures));
 *     co_return result;
 * }
 * @endcode
 */

#include <coroutine>
#include <optional>
#include <expected>
#include <memory>
#include <functional>
#include <exception>
#include <utility>

#include "promises.hpp"
#include "poller.hpp"

namespace NNet {

template<typename T> struct TFinalAwaiter;

template<typename T> struct TFuture;

/**
 * @brief Base promise type for coroutines.
 *
 * Provides the initial and final suspension behavior and stores the caller
 * coroutine's handle.
 *
 * @tparam T The type of the coroutine's return value.
 */
template<typename T>
struct TPromiseBase {
    std::suspend_never initial_suspend() { return {}; }
    TFinalAwaiter<T> final_suspend() noexcept;
    /// Handle to the caller coroutine (initialized to a no-operation coroutine).
    std::coroutine_handle<> Caller = std::noop_coroutine();
};

/**
 * @brief Promise for coroutines that return a value of type T.
 *
 * Stores either the result of the coroutine or an exception pointer if an error
 * occurs during execution.
 *
 * @tparam T The type of the return value.
 */
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
        ErrorOr = std::unexpected(std::current_exception());
    }

    /// Optional container that holds either the result or an exception.
    std::optional<std::expected<T, std::exception_ptr>> ErrorOr;
};

/**
 * @brief Base future type for coroutines.
 *
 * Manages the coroutine handle and provides basic mechanisms to await the
 * coroutine's completion.
 *
 * @tparam T The type of the result produced by the coroutine.
 */
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
            if (Coro) {
                Coro.destroy();
            }
            Coro = std::exchange(other.Coro, nullptr);
        }
        return *this;
    }

    ~TFutureBase() { if (Coro) { Coro.destroy(); } }

    bool await_ready() const {
        return Coro.promise().ErrorOr.has_value();
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

    void detach() {
        if (Coro) {
            Coro.promise().Caller = std::noop_coroutine();
        }
    }

    using promise_type = TPromise<T>;

protected:
    std::coroutine_handle<TPromise<T>> Coro = nullptr;
};

template<> struct TFuture<void>;

/**
 * @brief Owned coroutine handle that carries a result of type T.
 *
 * RAII: destroys the coroutine frame in `~TFuture`. Move-only.
 * `co_await future` suspends the caller until the coroutine finishes, then
 * returns `T` or rethrows a stored exception. `future.done()` polls
 * completion without suspending.
 *
 * @tparam T The type of the result.
 */
template<typename T>
struct TFuture : public TFutureBase<T> {
    T await_resume() {
        auto& errorOr = *this->Coro.promise().ErrorOr;
        if (errorOr.has_value()) {
            return std::move(errorOr.value());
        } else {
            std::rethrow_exception(errorOr.error());
        }
    }


    /**
     * @brief Applies a function to the result of the coroutine.
     *
     * The provided function is applied to the result, and the outcome is
     * wrapped in a new future.
     *
     * @tparam Func The type of the function.
     * @param func The function to apply.
     * @return A TFuture wrapping the result of the function call.
     */
    template<typename Func>
    auto Apply(Func func) -> TFuture<decltype(func(std::declval<T>()))> {
        auto prev = std::move(*this);
        auto ret = co_await prev;
        co_return func(ret);
    }

    /**
     * @brief Awaits the coroutine and ignores its result.
     *
     * This is useful when the result is not needed.
     *
     * @return TFuture<void> representing the completion.
     */
    TFuture<void> Ignore();
};

/**
 * @brief Promise specialization for coroutines that return void.
 */
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

/**
 * @brief Owned coroutine handle for coroutines that return void.
 *
 * Same ownership and exception-propagation semantics as `TFuture<T>`.
 * `co_await future` suspends the caller until done, then rethrows any
 * stored exception. Provides `Accept(func)` to chain a continuation.
 */
template<>
struct TFuture<void> : public TFutureBase<void> {
    void await_resume() {
        auto& errorOr = *this->Coro.promise().ErrorOr;
        if (errorOr) {
            std::rethrow_exception(errorOr);
        }
    }

    /**
     * @brief Registers a continuation to be executed after the coroutine completes.
     *
     * @tparam Func The type of the continuation function.
     * @param func The function to execute after completion.
     * @return TFuture<void> representing the continuation.
     */
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

/**
 * @brief Final awaiter for a coroutine.
 *
 * TFinalAwaiter is used during the final suspension of a coroutine. When the
 * coroutine completes, this awaiter ensures that the caller coroutine (whose
 * handle is stored in the promise) is resumed. This mechanism allows proper
 * chaining of asynchronous operations.
 *
 * @tparam T The return type of the coroutine.
 */
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

/**
 * @brief Awaits every future in order and collects their results.
 *
 * Futures are `co_await`-ed sequentially (not concurrently). The caller
 * suspends until each future finishes before moving to the next.
 *
 * @tparam T The type of each coroutine's result.
 * @param futures A vector of TFuture<T> objects (moved in).
 * @return TFuture<std::vector<T>> containing results in input order.
 */
template<typename T>
TFuture<std::vector<T>> All(std::vector<TFuture<T>>&& futures) {
    auto waiting = std::move(futures);
    std::vector<T> ret; ret.reserve(waiting.size());
    for (auto& f : waiting) {
        ret.emplace_back(std::move(co_await f));
    }
    co_return ret;
}

/**
 * @brief Awaits every void future in order until all have completed.
 *
 * @param futures A vector of TFuture<void> objects (moved in).
 * @return TFuture<void> that completes after all futures finish.
 */
inline TFuture<void> All(std::vector<TFuture<void>>&& futures) {
    auto waiting = std::move(futures);
    for (auto& f : waiting) {
        co_await f;
    }
    co_return;
}

/**
 * @brief Returns the result of whichever future completes first.
 *
 * If one future is already done, its result is returned without suspension.
 * Otherwise, all futures register this coroutine as their continuation; when
 * the first one resumes it, the remaining futures are abandoned (their frames
 * are destroyed when the vector goes out of scope).
 *
 * @tparam T The type of each coroutine's result.
 * @param futures A vector of TFuture<T> objects (moved in).
 * @return TFuture<T> with the result from the first completed future.
 */
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
    for (auto& f : all) {
        if (!f.done()) {
            f.detach();
        }
    }
    co_return std::find_if(all.begin(), all.end(), [](auto& f) { return f.done(); })->await_resume();
}

/**
 * @brief Completes when the first void future finishes; others are abandoned.
 *
 * @param futures A vector of TFuture<void> objects (moved in).
 * @return TFuture<void> that completes as soon as one future finishes.
 */
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
    for (auto& f : all) {
        if (!f.done()) {
            f.detach();
        }
    }
    co_return;
}

} // namespace NNet
