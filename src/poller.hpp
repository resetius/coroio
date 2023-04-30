#pragma once

#include <iostream>
#include <vector>
#include <map>
#include <queue>

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
        Changes_.emplace_back(TEventChange{fd, TEventChange::READ, h});
    }

    void AddWrite(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEventChange{fd, TEventChange::WRITE, h});
    }

    void RemoveEvent(int fd) {
        // TODO: resume waiting coroutines here
        MaxFd_ = std::max(MaxFd_, fd);
        Changes_.emplace_back(TEventChange{fd, TEventChange::READ|TEventChange::WRITE, {}});
    }

    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        auto now = TClock::now();
        auto next= now+duration;
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
        return TAwaitable{this,next};
    }

    void Wakeup(TEventChange&& change) {
        change.Handle.resume();
        if (Changes_.empty() || !Changes_.back().Match(change)) {
            change.Handle = {};
            Changes_.emplace_back(std::move(change));
        }
    }

    void WakeupReadyHandles() {
        int i = 0;
        for (auto&& ev : ReadyEvents_) {
            Wakeup(std::move(ev));
        }
    }

protected:
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
                ReadyEvents_.emplace_back(TEventChange{-1, 0, timer.Handle});
            }

            prevFd = timer.Fd;
            Timers_.pop();
        }
        LastTimersProcessTime_ = now;
    }

    std::map<int,TEvent> Events_;  // changes
    int MaxFd_ = 0;
    std::vector<std::tuple<int,THandle>> NewReads_;
    std::vector<std::tuple<int,THandle>> NewWrites_;
    std::queue<int> DelReads_;
    std::queue<int> DelWrites_;
    std::vector<TEventChange> Changes_;
    std::vector<TEventChange> ReadyEvents_;
    std::vector<THandle> ReadyHandles_;
    std::priority_queue<TTimer> Timers_;
    TTime LastTimersProcessTime_;
};

} // namespace NNet
