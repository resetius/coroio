#pragma once

#include <poll.h>
#include <assert.h>

#include <iostream>

#include "base.hpp"
#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TPoll: public TPollerBase {
public:
    using TSocket = NNet::TSocket;

    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        int timeout = GetMillis(TClock::now(), deadline);

        if (InEvents_.size() <= MaxFd_) {
            InEvents_.resize(MaxFd_+1, std::make_tuple(TEvent{}, -1));
        }

        auto dropFlag = [&]<int flag>(std::queue<int>& q) {
            while (!q.empty()) {
                auto k = q.front(); q.pop();
                auto& [ev, idx] = InEvents_[k];
                if (idx != -1) {
                    Fds_[idx].events &= ~flag;
                    if (Fds_[idx].events == 0) {
                        std::swap(Fds_[idx], Fds_.back());
                        std::get<1>(InEvents_[Fds_[idx].fd]) = idx;
                        Fds_.back() = {};
                    }

                    if constexpr(flag == POLLIN) {
                        ev.Read = {};
                    } else {
                        ev.Write = {};
                    }
                }
            }
        };

        auto addFlag = [&]<int flag>(std::vector<std::tuple<int,THandle>>& q) {
            for (auto& [k, h] : q) {
                auto& [ev, idx] = InEvents_[k];
                if (idx == -1) {
                    idx = Fds_.size();
                    Fds_.emplace_back(pollfd{});
                }

                pollfd& pev = Fds_[idx];
                pev.fd = k;
                pev.events |= flag;
                if constexpr(flag == POLLIN) {
                    ev.Read = h;
                } else {
                    ev.Write = h;
                }
            }
        };

        dropFlag.template operator()<POLLIN>(DelReads_);
        dropFlag.template operator()<POLLOUT>(DelWrites_);
        addFlag.template operator()<POLLIN>(NewReads_);
        addFlag.template operator()<POLLOUT>(NewWrites_);

        while (!Fds_.empty() && !Fds_.back().events) {
            Fds_.pop_back();
        }

        Reset();

        if (poll(&Fds_[0], Fds_.size(), timeout) < 0) {
            throw std::system_error(errno, std::generic_category(), "poll");
        }

        for (auto& pev : Fds_) {
            auto& [ev, _] = InEvents_[pev.fd];
            if (pev.revents & POLLIN) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
                DelReads_.emplace(pev.fd);
            }
            if (pev.revents & POLLOUT) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
                DelWrites_.emplace(pev.fd);
            }
            if (pev.revents & POLLHUP) {
                if (ev.Read) {
                    ReadyHandles_.emplace_back(std::move(ev.Read));
                    ev.Read = {};
                    DelReads_.emplace(pev.fd);
                }
                if (ev.Write) {
                    ReadyHandles_.emplace_back(std::move(ev.Write));
                    ev.Write = {};
                    DelWrites_.emplace(pev.fd);
                }
            }
        }

        ProcessTimers();
    }

private:
    std::vector<std::tuple<TEvent,int>> InEvents_; // event + index in Fds
    std::vector<pollfd> Fds_;
};

} // namespace NNet
