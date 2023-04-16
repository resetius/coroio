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
        Timers_.emplace(TTimer{deadline, fd, {}}); // insert empty timer befor existing
        auto it = Events_.find(fd);
        if (it == Events_.end()) {
            return false;
        } else {
            bool r = it->second.TimeoutFired;
            it->second.TimeoutFired = false;
            return r;
        }
    }

    void AddRead(int fd, THandle h) {
        Events_[fd].Read = std::move(h);
    }

    void AddWrite(int fd, THandle h) {
        Events_[fd].Write = std::move(h);
    }

    void RemoveEvent(int fd) {
        // TODO: resume waiting coroutines here
        Events_[fd] = {};
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

protected:
    void ProcessTimers() {
        auto now = TClock::now();
        int prevFd = -1;
        while (!Timers_.empty()&&Timers_.top().Deadline <= now) {
            TTimer timer = std::move(Timers_.top());            

            if ((prevFd == -1 || prevFd != timer.Fd) && timer.Handle) { // skip removed timers
                ReadyHandles_.emplace_back(timer.Handle);

                if (timer.Fd >= 0) {
                    Events_[timer.Fd].TimeoutFired = true;
                }
            }

            prevFd = timer.Fd;
            Timers_.pop();
        }
    }

    std::map<int,TEvent> Events_;
    std::priority_queue<TTimer> Timers_;
    std::vector<THandle> ReadyHandles_;
};

} // namespace NNet
