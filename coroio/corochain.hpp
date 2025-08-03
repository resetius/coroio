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

    using promise_type = TPromise<T>;

protected:
    std::coroutine_handle<TPromise<T>> Coro = nullptr;
};

template<> struct TFuture<void>;

/**
 * @brief Future type for coroutines returning a value of type T.
 *
 * Provides mechanisms to await and retrieve the result of a coroutine.
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
 * @brief Future specialization for coroutines that return void.
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
 * @brief Awaits the completion of all futures and collects their results.
 *
 * @tparam T The type of each coroutine's result.
 * @param futures A vector of TFuture<T> objects.
 * @return TFuture<std::vector<T>> containing the results from all coroutines.
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
 * @brief Awaits the completion of all void-returning coroutines.
 *
 * @param futures A vector of TFuture<void> objects.
 * @return TFuture<void> representing the completion of all coroutines.
 */
inline TFuture<void> All(std::vector<TFuture<void>>&& futures) {
    auto waiting = std::move(futures);
    for (auto& f : waiting) {
        co_await f;
    }
    co_return;
}

/**
 * @brief Awaits the completion of any one of the given futures and returns its result.
 *
 * If one of the futures has already finished, its result is returned immediately.
 * Otherwise, the current coroutine is suspended until one of the futures completes.
 *
 * @tparam T The type of the coroutine's result.
 * @param futures A vector of TFuture<T> objects.
 * @return TFuture<T> with the result from the first completed coroutine.
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
    co_return std::find_if(all.begin(), all.end(), [](auto& f) { return f.done(); })->await_resume();
}

/**
 * @brief Awaits the completion of any one of the void-returning coroutines.
 *
 * @param futures A vector of TFuture<void> objects.
 * @return TFuture<void> representing the completion of one of the coroutines.
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
    co_return;
}

} // namespace NNet
