#pragma once

#include <poll.h>
#include <assert.h>

#include <iostream>

#include "base.hpp"
#include "poller.hpp"

namespace NNet {

class TPoll: public TPollerBase {
public:
    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        int timeout = GetMillis(TClock::now(), deadline);

        for (auto& [k, ev] : Events_) {
            if (InEvents_.size() <= k) {
                InEvents_.resize(k+1, {{}, -1});
            }
            auto& [old_ev, idx] = InEvents_[k];
            pollfd pev = {.fd = k, .events = 0, .revents = 0};
            if (ev.Read) {
                pev.events |= POLLIN;
            }
            if (ev.Write) {
                pev.events |= POLLOUT;
            }

            if (pev.events) {
                if (idx == -1) {
                    idx = Fds_.size(); 
                    Fds_.emplace_back(std::move(pev));
                } else {
                    Fds_[idx] = pev;
                }
                old_ev = ev;
            } else {
                if (idx != -1) {                    
                    std::swap(Fds_[idx], Fds_.back());
                    std::get<1>(InEvents_[Fds_[idx].fd]) = idx;
                    Fds_.back() = {};
                }

                while (!Fds_.empty() && !Fds_.back().events) {
                    Fds_.pop_back();
                }
            }        
        }

        Events_.clear();
        ReadyHandles_.clear();

        if (poll(&Fds_[0], Fds_.size(), timeout) < 0) {
            throw std::system_error(errno, std::generic_category(), "poll");
        }

        for (auto& pev : Fds_) {
            auto& [ev, _] = InEvents_[pev.fd];
            bool changed = false;
            if (pev.revents & POLLIN) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
                changed |= true;
            }
            if (pev.revents & POLLOUT) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
                changed |= true;
            }
            if (pev.revents & POLLHUP) {
                changed |= true;
                if (ev.Read) {
                    ReadyHandles_.emplace_back(std::move(ev.Read));
                    ev.Read = {};
                }
                if (ev.Write) {
                    ReadyHandles_.emplace_back(std::move(ev.Write));
                    ev.Write = {};
                }
            }

            if (changed) {
                Events_.emplace(pev.fd, ev);
            }
        }

        ProcessTimers();
    }

private:
    std::vector<std::tuple<TEvent,int>> InEvents_; // event + index in Fds
    std::vector<pollfd> Fds_;
};

} // namespace NNet
