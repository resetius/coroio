#pragma once

#include <chrono>
#include <iostream>
#include <vector>
#include <map>
#include <queue>
#include <assert.h>

#include "base.hpp"

#ifdef Yield
#undef Yield
#endif

namespace NNet {

class TPollerBase {
public:
    TPollerBase() = default;
    TPollerBase(const TPollerBase& ) = delete;
    TPollerBase& operator=(const TPollerBase& ) = delete;

    unsigned AddTimer(TTime deadline, THandle h) {
        Timers_.emplace(TTimer{deadline, TimerId_, h});
        return TimerId_++;
    }

    bool RemoveTimer(unsigned timerId, TTime deadline) {
        bool fired = timerId == LastFiredTimer_;
        if (!fired) {
            Timers_.emplace(TTimer{deadline, timerId, {}}); // insert empty timer before existing
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

    void AddRemoteHup(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::RHUP, h});
    }

    void RemoveEvent(int fd) {
        // TODO: resume waiting coroutines here
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEvent{fd, TEvent::READ|TEvent::WRITE|TEvent::RHUP, {}});
    }

    void RemoveEvent(THandle /*h*/) {
        // TODO: Add new vector for this type of removing
        // Will be called in destuctor of unfinished futures
    }

    auto Sleep(TTime until) {
        struct TAwaitableSleep {
            TAwaitableSleep(TPollerBase* poller, TTime n)
                : poller(poller)
                , n(n)
            { }
            ~TAwaitableSleep() {
                if (poller) {
                    poller->RemoveTimer(timerId, n);
                }
            }

            TAwaitableSleep(TAwaitableSleep&& other)
                : poller(other.poller)
                , n(other.n)
            {
                other.poller = nullptr;
            }

            TAwaitableSleep(const TAwaitableSleep&) = delete;
            TAwaitableSleep& operator=(const TAwaitableSleep&) = delete;

            bool await_ready() {
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) {
                timerId = poller->AddTimer(n, h);
            }

            void await_resume() { poller = nullptr; }

            TPollerBase* poller;
            TTime n;
            unsigned timerId = 0;
        };

        return TAwaitableSleep{this,until};
    }

    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        return Sleep(TClock::now() + duration);
    }

    auto Yield() {
        return Sleep(TTime{});
    }

    void Wakeup(TEvent&& change) {
        auto index = Changes_.size();
        change.Handle.resume();
        if (change.Fd >= 0) {
            bool matched = false;
            for (; index < Changes_.size() && !matched; index++) {
                matched = Changes_[index].Match(change);
            }
            if (!matched) {
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

    auto TimersSize() const {
        return Timers_.size();
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
        timespec ts;
        ts.tv_sec = p1.count();
        ts.tv_nsec = p2.count();
        return ts;
    }

    void Reset() {
        ReadyEvents_.clear();
        Changes_.clear();
        MaxFd_ = 0;
    }

    void ProcessTimers() {
        auto now = TClock::now();
        bool first = true;
        unsigned prevId = 0;

        while (!Timers_.empty()&&Timers_.top().Deadline <= now) {
            TTimer timer = Timers_.top(); Timers_.pop();

            if ((first || prevId != timer.Id) && timer.Handle) { // skip removed timers
                LastFiredTimer_ = timer.Id;
                timer.Handle.resume();
            }

            first = false;
            prevId = timer.Id;
        }

        LastTimersProcessTime_ = now;
    }

    int MaxFd_ = -1;
    std::vector<TEvent> Changes_;
    std::vector<TEvent> ReadyEvents_;
    unsigned TimerId_ = 0;
    std::priority_queue<TTimer> Timers_;
    TTime LastTimersProcessTime_;
    unsigned LastFiredTimer_ = (unsigned)(-1);
    std::chrono::milliseconds MaxDuration_ = std::chrono::milliseconds(100);
    timespec MaxDurationTs_ = GetMaxDuration(MaxDuration_);
};

} // namespace NNet
