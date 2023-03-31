#pragma once

#include <unordered_map>
#include <vector>
#include <coroutine>
#include <chrono>
#include <iostream>

#include <cstdint>

#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

using i64 = int64_t;

namespace NNet {

class TEvent {
public:
    void Handle() { }
};

class TTimer {
public:
    using TTime = std::chrono::steady_clock::time_point;

    TTimer(i64 id, TTime end)
        : Id_(id)
        , EndTime_(end)
    { }

    i64 Id() const {
        return Id_;
    }

    void Handle() {
        H_.resume();
    }

    auto Awaitable() {
        struct awaitable {
            std::coroutine_handle<>* H;

            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                *H = h;
            }
            void await_resume() { }
        };

        return awaitable{&H_};
    }

    auto EndTime() const {
        return EndTime_;
    }

private:
    i64 Id_;
    TTime EndTime_;
    std::coroutine_handle<> H_;
};

class TSelect {
    std::unordered_map<i64,TTimer> Timers_;
    std::vector<TTimer> ReadyTimers_;
    bool Running_ = true;

public:
    TTimer& Add(TTimer&& timer) {
        return Timers_.emplace(timer.Id(), std::move(timer)).first->second;
    }

    void Add(TEvent&& event) { }
    bool Ready() {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100;
        select(0, NULL, NULL, NULL, &tv); // TODO: return code
        auto now = std::chrono::steady_clock::now();
        ReadyTimers_.clear();
        for (auto& [k, v]: Timers_) {
            if (now >= v.EndTime()) {
                ReadyTimers_.emplace_back(std::move(v));
            }
        }
        for (auto& v : ReadyTimers_) {
            Timers_.erase(v.Id());
        };
        return Running_;
    }

    auto& ReadyTimers() {
        return ReadyTimers_;
    }
};

class TAddress {
public:
    TAddress(const std::string& addr, int port) { }
};

class TLoop {
    TSelect Select;
    i64 TimerId = 0;

public:
    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        auto now = std::chrono::steady_clock::now();
        auto next= now+duration;
        TTimer t(TimerId++, next);
        return Select.Add(std::move(t)).Awaitable();
    }

    void Loop() {
        std::vector<TEvent> events;

        while (Select.Ready()) {
            for (auto& ev : Select.ReadyTimers()) {
                ev.Handle();
            }
        }
    }
};

class TSocket {
public:
    TSocket(TAddress&& addr, TLoop* loop)
        : Loop_(loop)
    { }
    TSocket(const TAddress& addr, TLoop* loop)
        : Loop_(loop)
    { }
    TSocket(TLoop* loop)
        : Loop_(loop)
    {

    }
    auto Connect() {
        return std::suspend_always();
    }

    auto Read(char* buf, size_t size) {
        int ret = read(Fd_, buf, size);
        int err;
        if (ret < 0) {
            err = errno;
        }
        struct awaitable {
            bool await_ready() {
                return (ready = (ret >= 0));
            }

            void await_suspend(std::coroutine_handle<> h) {
            }

            int await_resume() {
                return ret;
            }

            int ret, err, ready;
        };
        return awaitable{ret,err,false};
    }

    auto Write(char* buf, size_t size) {
        return std::suspend_always();
    }

    auto Accept() {
        struct awaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
            }
            TSocket await_resume() {
                return TSocket{loop};
            }

            TLoop* loop;
        };

        return awaitable{Loop_};
    }

    void Bind() { }
    void Listen() { }

private:
    int Fd_;
    TLoop* Loop_;
};

} /* namespace NNet */
