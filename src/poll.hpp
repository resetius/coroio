#pragma once

#include <poll.h>
#include <iostream>

#include "base.hpp"
#include "poller.hpp"

namespace NNet {

class TPoll: public TPollerBase {
public:
    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        auto tv = GetTimeval(TClock::now(), deadline);
        int timeout = tv.tv_sec * 1000 + tv.tv_usec / 1000000;

        Fds_.clear();
        for (auto& [k, ev] : Events_) {
            pollfd pev = {.fd = k, .events = 0, .revents = 0};
            if (ev.Read) {
                pev.events |= POLLIN;
            }
            if (ev.Write) {
                pev.events |= POLLOUT;
            }
            Fds_.emplace_back(std::move(pev));
        }

        if (poll(&Fds_[0], Fds_.size(), timeout) < 0) { throw TSystemError(); }

        ReadyHandles_.clear();
        for (auto& pev : Fds_) {
            auto& ev = Events_[pev.fd];
            if (pev.revents & POLLIN) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
            }
            if (pev.revents & POLLOUT) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
            }
            if (pev.revents & POLLHUP) {
                if (ev.Read) {
                    ReadyHandles_.emplace_back(std::move(ev.Read));
                    ev.Read = {};
                }
                if (ev.Write) {
                    ReadyHandles_.emplace_back(std::move(ev.Write));
                    ev.Write = {};
                }
            }
        }

        ProcessTimers();
    }

private:
    std::vector<pollfd> Fds_;
};

} // namespace NNet
