#pragma once

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
        NewReads_.emplace_back(fd, h);
        while (!DelReads_.empty() && DelReads_.back() == fd) {
            DelReads_.pop();
        }
    }

    void AddWrite(int fd, THandle h) {
        MaxFd_ = std::max(MaxFd_, fd);
        NewWrites_.emplace_back(fd, h);
        while (!DelWrites_.empty() && DelWrites_.back() == fd) {
            DelWrites_.pop();
        }
    }

    void RemoveEvent(int fd) {
        // TODO: resume waiting coroutines here
        MaxFd_ = std::max(MaxFd_, fd);
        DelReads_.emplace(fd);
        DelWrites_.emplace(fd);
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

    auto& ReadyHandles() {
        return ReadyHandles_;
    }

    void WakeupReadyHandles() {
        for (auto& ev : ReadyHandles_) {
            ev.resume();
        }
    }

protected:
    void Reset() {
        NewReads_.clear();
        NewWrites_.clear();
        ReadyHandles_.clear();
        MaxFd_ = 0;
    }

    void ProcessTimers() {
        auto now = TClock::now();
        int prevFd = -1;
        while (!Timers_.empty()&&Timers_.top().Deadline <= now) {
            TTimer timer = std::move(Timers_.top());

            if ((prevFd == -1 || prevFd != timer.Fd) && timer.Handle) { // skip removed timers
                ReadyHandles_.emplace_back(timer.Handle);
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
    std::priority_queue<TTimer> Timers_;
    std::vector<THandle> ReadyHandles_;
    TTime LastTimersProcessTime_;
};

} // namespace NNet
