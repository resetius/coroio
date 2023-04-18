#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include "base.hpp"
#include "poller.hpp"

namespace NNet {

class TEPoll: public TPollerBase {
public:
    TEPoll()
        : Fd_(epoll_create1(EPOLL_CLOEXEC))
    {
        if (Fd_ < 0) { 
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
    }

    ~TEPoll()
    {
        if (Fd_ >= 0) { close(Fd_); }
    }

    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        int timeout = GetMillis(TClock::now(), deadline);

        for (auto& [k, ev] : Events_) {
            epoll_event eev = {};
            bool changed = false;
            eev.data.fd = k;
            if (InEvents_.size() <= k) {
                InEvents_.resize(k+1);
            }
            auto& old_ev = InEvents_[k];

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
                if (epoll_ctl(Fd_, EPOLL_CTL_DEL, eev.data.fd, nullptr) < 0) {
                    if (errno != EBADF) { // closed descriptor after TSocket -> close
                        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                    }
                }
            } else if (!old_ev.Write && !old_ev.Read) {
                old_ev = ev;
                if (epoll_ctl(Fd_, EPOLL_CTL_ADD, eev.data.fd, &eev) < 0) {
                    throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                }
            } else if (changed) {
                old_ev = ev;
                if (epoll_ctl(Fd_, EPOLL_CTL_MOD, eev.data.fd, &eev) < 0) {
                    throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                }
            }
        }

        Events_.clear();
        ReadyHandles_.clear();

        OutEvents_.resize(std::max<size_t>(1, InEvents_.size()));

        int nfds;
        if ((nfds =  epoll_wait(Fd_, &OutEvents_[0], OutEvents_.size(), timeout)) < 0) { 
            throw std::system_error(errno, std::generic_category(), "epoll_wait");
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = OutEvents_[i].data.fd;
            auto ev = InEvents_[fd];
            if (OutEvents_[i].events & EPOLLIN) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
            }
            if (OutEvents_[i].events & EPOLLOUT) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
            }
            if (OutEvents_[i].events & EPOLLHUP) {
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
    int Fd_;
    std::vector<TEvent> InEvents_;       // all events in epoll
    std::vector<epoll_event> OutEvents_; // events out from epoll_wait
};

} // namespace NNet
