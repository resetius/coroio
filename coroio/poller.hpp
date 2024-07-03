#pragma once

#include <chrono>
#include <iostream>
#include <vector>
#include <map>
#include <queue>
#include <assert.h>

#include "base.hpp"

namespace NNet {

class TPollerBase {
public:
    TPollerBase() = default;
    TPollerBase(const TPollerBase& ) = delete;
    TPollerBase& operator=(const TPollerBase& ) = delete;

    void AddTimer(int fd, TTime deadline, THandle h) {
        Timers_.emplace(TTimer{deadline, fd, h});
    }

    bool RemoveTimer(int fd, TTime deadline) {
        bool fired = deadline < LastTimersProcessTime_;
        if (!fired) {
            Timers_.emplace(TTimer{deadline, fd, {}}); // insert empty timer before existing
        }
        return fired;
    }

    void AddRead(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::READ, h});
    }

    void AddWrite(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::WRITE, h});
    }

    void RemoveEvent(int fd) {
        // TODO: resume waiting coroutines here
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::READ|TEvent::WRITE, {}});
    }

    auto Sleep(TTime until) {
        struct TAwaitable {
            bool await_ready() {
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) {
                poller->AddTimer(-1, n, h);
            }

            void await_resume() { }

            TPollerBase* poller;
            TTime n;
        };
        return TAwaitable{this,until};
    }

    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        return Sleep(TClock::now() + duration);
    }

    auto Yield() {
        return Sleep(TTime{});
    }

    void Wakeup(TEvent&& change) {
        change.Handle.resume();
        if (Changes_.empty() || !Changes_.back().Match(change)) {
            if (change.Fd >= 0) {
                change.Handle = {};
                Changes_.emplace_back(std::move(change));
            }
        }
    }

    void WakeupReadyHandles() {
        for (auto&& ev : ReadyEvents_) {
            Wakeup(std::move(ev));
        }
    }

    void SetMaxDuration(std::chrono::milliseconds maxDuration) {
        MaxDuration_ = maxDuration;
        MaxDurationTs_ = GetMaxDuration(MaxDuration_);
    }

protected:
    timespec GetTimeout() const {
        return Timers_.empty()
            ? MaxDurationTs_
            : Timers_.top().Deadline == TTime{}
                ? timespec {0, 0}
                : GetTimespec(TClock::now(), Timers_.top().Deadline, MaxDuration_);
    }

    static constexpr timespec GetMaxDuration(std::chrono::milliseconds duration) {
        auto p1 = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto p2 = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - p1);
        return {p1.count(), p2.count()};
    }

    void Reset() {
        ReadyEvents_.clear();
        Changes_.clear();
        MaxFd_ = 0;
    }

    void ProcessTimers() {
        auto now = TClock::now();
        int prevFd = -1;
        while (!Timers_.empty()&&Timers_.top().Deadline <= now) {
            TTimer timer = std::move(Timers_.top());

            if ((prevFd == -1 || prevFd != timer.Fd) && timer.Handle) { // skip removed timers
                ReadyEvents_.emplace_back(TEvent{-1, 0, timer.Handle});
            }

            prevFd = timer.Fd;
            Timers_.pop();
        }
        LastTimersProcessTime_ = now;
    }

    int MaxFd_ = 0;
    std::vector<TEvent> Changes_;
    std::vector<TEvent> ReadyEvents_;
    std::priority_queue<TTimer> Timers_;
    TTime LastTimersProcessTime_;
    std::chrono::milliseconds MaxDuration_ = std::chrono::milliseconds(100);
    timespec MaxDurationTs_ = GetMaxDuration(MaxDuration_);
};

} // namespace NNet
