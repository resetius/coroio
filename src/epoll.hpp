#pragma once

#include <asm-generic/errno-base.h>
#include <cerrno>
#include <sys/epoll.h>
#include <unistd.h>

#include <iostream>

#include "base.hpp"
#include "poller.hpp"

namespace NNet {

class TEPoll: public TPollerBase {
public:
    TEPoll()
        : EpollFd_(epoll_create1(EPOLL_CLOEXEC))
    {
        if (EpollFd_ < 0) { throw TSystemError(); }
    }

    ~TEPoll()
    {
        if (EpollFd_ >= 0) { close(EpollFd_); }
    }

    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        auto tv = GetTimeval(TClock::now(), deadline);
        int timeout = tv.tv_sec * 1000 + tv.tv_usec / 1000000;

        for (auto& [k, ev] : Events_) {
            epoll_event eev = {};
            eev.data.fd = k;
            if (ev.Read && ev.Write) {
                eev.events = EPOLLIN | EPOLLOUT | EPOLLONESHOT;
            } else if (ev.Read) {
                eev.events = EPOLLIN | EPOLLONESHOT;
            } else if (ev.Write) {
                eev.events = EPOLLOUT | EPOLLONESHOT;
            }
            if (epoll_ctl(EpollFd_, EPOLL_CTL_ADD, k, &eev) < 0) { 
                if (errno == EEXIST) {
                    if (epoll_ctl(EpollFd_, EPOLL_CTL_MOD, k, &eev) < 0) {
                        throw TSystemError(); 
                    }
                } else {
                    throw TSystemError(); 
                }
            }
        }

        EpollEvents_.resize(std::max<size_t>(1, Events_.size()));

        int nfds;
        if ((nfds =  epoll_wait(EpollFd_, &EpollEvents_[0], EpollEvents_.size(), timeout)) < 0) { throw TSystemError(); }

        ReadyHandles_.clear();
        for (int i = 0; i < nfds; ++i) {
            auto& ev = Events_[EpollEvents_[i].data.fd];
            if (EpollEvents_[i].events & EPOLLIN) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
            }
            if (EpollEvents_[i].events & EPOLLOUT) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
            }
            if (EpollEvents_[i].events & EPOLLHUP) {
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
    int EpollFd_;
    std::vector<epoll_event> EpollEvents_;
};

} // namespace NNet
