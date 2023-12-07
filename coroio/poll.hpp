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
    using TFileHandle = NNet::TFileHandle;

    void Poll() {
        auto ts = GetTimeout();

        if (InEvents_.size() <= MaxFd_) {
            InEvents_.resize(MaxFd_+1, std::make_tuple(THandlePair{}, -1));
        }

        for (const auto& ch : Changes_) {
            int fd = ch.Fd;
            auto& [ev, idx] = InEvents_[fd];
            if (ch.Handle) {
                if (idx == -1) {
                    idx = Fds_.size();
                    Fds_.emplace_back(pollfd{});
                }
                pollfd& pev = Fds_[idx];
                pev.fd = fd;
                if (ch.Type & TEvent::READ) {
                    pev.events |= POLLIN;
                    ev.Read = ch.Handle;
                }
                if (ch.Type & TEvent::WRITE) {
                    pev.events |= POLLOUT;
                    ev.Write = ch.Handle;
                }
            } else if (idx != -1) {
                if (ch.Type & TEvent::READ) {
                    Fds_[idx].events &= ~POLLIN;
                    ev.Read = {};
                }
                if (ch.Type & TEvent::WRITE) {
                    Fds_[idx].events &= ~POLLOUT;
                    ev.Write = {};
                }
                if (Fds_[idx].events == 0) {
                    std::swap(Fds_[idx], Fds_.back());
                    std::get<1>(InEvents_[Fds_[idx].fd]) = idx;
                    Fds_.back().events = 0;
                    idx = -1;
                }
            }
        }

        while (!Fds_.empty() && !Fds_.back().events) {
            Fds_.pop_back();
        }

        Reset();

        if (ppoll(&Fds_[0], Fds_.size(), &ts, nullptr) < 0) {
            throw std::system_error(errno, std::generic_category(), "poll");
        }

        for (auto& pev : Fds_) {
            auto [ev, _] = InEvents_[pev.fd];
            if (pev.revents & POLLIN) {
                ReadyEvents_.emplace_back(TEvent{pev.fd, TEvent::READ, ev.Read}); ev.Read = {};
            }
            if (pev.revents & POLLOUT) {
                ReadyEvents_.emplace_back(TEvent{pev.fd, TEvent::WRITE, ev.Write}); ev.Write = {};
            }
            if (pev.revents & POLLHUP) {
                if (ev.Read) {
                    ReadyEvents_.emplace_back(TEvent{pev.fd, TEvent::READ, ev.Read});
                }
                if (ev.Write) {
                    ReadyEvents_.emplace_back(TEvent{pev.fd, TEvent::WRITE, ev.Write});
                }
            }
        }

        ProcessTimers();
    }

private:
    std::vector<std::tuple<THandlePair,int>> InEvents_; // event + index in Fds
    std::vector<pollfd> Fds_;
};

} // namespace NNet
