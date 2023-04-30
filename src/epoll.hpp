#pragma once

#include <asm-generic/errno-base.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "base.hpp"
#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TEPoll: public TPollerBase {
public:
    using TSocket = NNet::TSocket;

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

        if (InEvents_.size() <= MaxFd_) {
            InEvents_.resize(MaxFd_+1);
        }

        auto addFlag = [&]<int flag>(std::vector<std::tuple<int,THandle>>& q) {
            for (auto& [k, h] : q) {
                auto& ev = InEvents_[k];
                epoll_event eev = {};
                eev.data.fd = k;

                bool changed = false;
                bool newEv = !ev.Read && !ev.Write;
                if constexpr(flag == EPOLLIN) {
                    changed = ev.Read != h;
                    ev.Read = h;
                } else {
                    changed = ev.Write != h;
                    ev.Write = h;
                }
                if (ev.Read) {
                    eev.events |= EPOLLIN;
                }
                if (ev.Write) {
                    eev.events |= EPOLLOUT;
                }

                if (newEv) {
                    if (epoll_ctl(Fd_, EPOLL_CTL_ADD, eev.data.fd, &eev) < 0) {
                        if (errno == EEXIST) {
                            if (epoll_ctl(Fd_, EPOLL_CTL_MOD, eev.data.fd, &eev) < 0) {
                                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                            }
                        } else {
                            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                        }
                    }
                } else if (changed) {
                    if (epoll_ctl(Fd_, EPOLL_CTL_MOD, eev.data.fd, &eev) < 0) {
                        if (errno == ENOENT) {
                            if (epoll_ctl(Fd_, EPOLL_CTL_ADD, eev.data.fd, &eev) < 0) {
                                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                            }
                        } else {
                            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                        }
                    }
                }
            }
        };

        auto dropFlag = [&]<int flag>(std::queue<int>& q) {
            while (!q.empty()) {
                int k = q.front(); q.pop();
                auto& ev = InEvents_[k];
                if constexpr(flag == EPOLLIN) {
                    ev.Read = {};
                } else {
                    ev.Write = {};
                }
                if (!ev.Read && !ev.Write) {
                    if (epoll_ctl(Fd_, EPOLL_CTL_DEL, k, nullptr) < 0) {
                        if (errno != EBADF) { // closed descriptor after TSocket -> close
                            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                        }
                    }
                } else {
                    epoll_event eev = {};
                    eev.data.fd = k;
                    if (ev.Read) {
                        eev.events |= EPOLLIN;
                    }
                    if (ev.Write) {
                        eev.events |= EPOLLOUT;
                    }
                    if (epoll_ctl(Fd_, EPOLL_CTL_MOD, k, &eev) < 0) {
                        if (errno == ENOENT) {
                            if (epoll_ctl(Fd_, EPOLL_CTL_ADD, k, &eev) < 0) {
                                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                            }
                        } else if (errno != EBADF) {
                            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                        }
                    }
                }
            }
        };

        dropFlag.template operator()<EPOLLIN>(DelReads_);
        dropFlag.template operator()<EPOLLOUT>(DelWrites_);
        addFlag.template operator()<EPOLLIN>(NewReads_);
        addFlag.template operator()<EPOLLOUT>(NewWrites_);

        Reset();

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
                DelReads_.emplace(fd);
            }
            if (OutEvents_[i].events & EPOLLOUT) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
                DelWrites_.emplace(fd);
            }
            if (OutEvents_[i].events & EPOLLHUP) {
                if (ev.Read) {
                    ReadyHandles_.emplace_back(std::move(ev.Read));
                    ev.Read = {};
                    DelReads_.emplace(fd);
                }
                if (ev.Write) {
                    ReadyHandles_.emplace_back(std::move(ev.Write));
                    ev.Write = {};
                    DelWrites_.emplace(fd);
                }
            }
        }

        ProcessTimers();
    }

private:
    int Fd_;
    std::vector<TEvent> InEvents_;       // all events in epoll
    std::vector<epoll_event> OutEvents_; // events out from epoll_wait
};

} // namespace NNet
