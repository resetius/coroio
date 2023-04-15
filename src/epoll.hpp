#pragma once

#include <sys/epoll.h>
#include <unistd.h>

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
        int timeout = GetMillis(TClock::now(), deadline);

        EpollModEvents_.clear();
        EpollNewEvents_.clear();
        EpollDelEvents_.clear();
        for (auto& [k, ev] : Events_) {
            epoll_event eev = {};
            bool changed = false;
            eev.data.fd = k;
            EpollInEvents_.resize(k+1);
            auto& old_ev = EpollInEvents_[k];

            if (ev.Read) {
                eev.events |= EPOLLIN;
                changed |= !old_ev.Read;
            }
            if (ev.Write) {
                eev.events |= EPOLLOUT;
                changed |= !old_ev.Write;
            }
            if (!ev.Write && !ev.Read) {
                old_ev = ev;
                EpollDelEvents_.emplace_back(eev);
            } else if (!old_ev.Write && !old_ev.Read) {
                old_ev = ev;
                EpollNewEvents_.emplace_back(eev);
            } else if (changed) {
                old_ev = ev;
                EpollModEvents_.emplace_back(eev);
            }
        }
        for (auto& eev : EpollNewEvents_) {
            if (epoll_ctl(EpollFd_, EPOLL_CTL_ADD, eev.data.fd, &eev) < 0) {
                throw TSystemError();
            }
        }
        for (auto& eev : EpollModEvents_) {
            if (epoll_ctl(EpollFd_, EPOLL_CTL_MOD, eev.data.fd, &eev) < 0) {
                throw TSystemError();
            }
        }
        for (auto& eev : EpollDelEvents_) {
            if (epoll_ctl(EpollFd_, EPOLL_CTL_DEL, eev.data.fd, nullptr) < 0) {
                if (errno != EBADF) { // closed descriptor after TSocket -> close
                    throw TSystemError();
                }
            }
            EpollInEvents_[eev.data.fd] = {};
        }

        Events_.clear();
        EpollOutEvents_.resize(std::max<size_t>(1, EpollInEvents_.size()));

        int nfds;
        if ((nfds =  epoll_wait(EpollFd_, &EpollOutEvents_[0], EpollOutEvents_.size(), timeout)) < 0) { throw TSystemError(); }

        ReadyHandles_.clear();
        for (int i = 0; i < nfds; ++i) {
            int fd = EpollOutEvents_[i].data.fd;
            auto ev = EpollInEvents_[fd];
            if (EpollOutEvents_[i].events & EPOLLIN) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
            }
            if (EpollOutEvents_[i].events & EPOLLOUT) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
            }
            if (EpollOutEvents_[i].events & EPOLLHUP) {
                if (ev.Read) {
                    ReadyHandles_.emplace_back(std::move(ev.Read));
                    ev.Read = {};
                }
                if (ev.Write) {
                    ReadyHandles_.emplace_back(std::move(ev.Write));
                    ev.Write = {};
                }
            }

            Events_.emplace(fd, ev);
        }

        ProcessTimers();
    }

private:
    int EpollFd_;
    std::vector<TEvent> EpollInEvents_;       // all events in epool
    std::vector<epoll_event> EpollOutEvents_; // events out from epoll_wait
    std::vector<epoll_event> EpollNewEvents_; // new events for epoll_ctl
    std::vector<epoll_event> EpollModEvents_; // changed events for epoll_ctl
    std::vector<epoll_event> EpollDelEvents_; // changed events for epoll_ctl
};

} // namespace NNet
