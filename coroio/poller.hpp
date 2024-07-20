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

    unsigned AddTimer(int fd, TTime deadline, THandle h) {
        Timers_.emplace(TTimer{deadline, fd, TimerId_, h});
        return TimerId_++;
    }

    bool RemoveTimer(int fd, unsigned timerId, TTime deadline) {
        bool fired = deadline < LastTimersProcessTime_;
        if (!fired) {
            Timers_.emplace(TTimer{deadline, fd, timerId, {}}); // insert empty timer before existing
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
                    poller->RemoveTimer(-1, timerId, n);
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
                timerId = poller->AddTimer(-1, n, h);
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
        return {p1.count(), p2.count()};
    }

    void Reset() {
        ReadyEvents_.clear();
        Changes_.clear();
        MaxFd_ = 0;
    }
void prn() {
if (Timers_.empty()) { return; }
std::vector<TTimer> timers;
std::cout << "Timers:\n";
while (!Timers_.empty()) {
auto t = Timers_.top(); Timers_.pop();
timers.push_back(t);
std::cout << t.Id << " " << t.Deadline.time_since_epoch() << " " << (bool)t.Handle << "\n";
}

for (auto t : timers) {
    Timers_.push(t);
}
}
    void ProcessTimers() {
        auto now = TClock::now();
        int prevFd = -2;
        unsigned prevId = 0;

        prn();
        while (!Timers_.empty()&&Timers_.top().Deadline <= now) {
            TTimer timer = Timers_.top(); Timers_.pop();
            auto size = Timers_.size();

            if ((prevFd == -2 || prevId != timer.Id) && timer.Handle) { // skip removed timers
                timer.Handle.resume();
                // prn();
                // ReadyEvents_.emplace_back(TEvent{-1, 0, timer.Handle});
                if (size != Timers_.size()) {
                    break;
                }
            }

            prevId = timer.Id;
            prevFd = timer.Fd;
        }

        LastTimersProcessTime_ = now;
    }

    int MaxFd_ = 0;
    std::vector<TEvent> Changes_;
    std::vector<TEvent> ReadyEvents_;
    unsigned TimerId_ = 0;
    std::priority_queue<TTimer> Timers_;
    TTime LastTimersProcessTime_;
    std::chrono::milliseconds MaxDuration_ = std::chrono::milliseconds(100);
    timespec MaxDurationTs_ = GetMaxDuration(MaxDuration_);
};

} // namespace NNet
