#ifdef __linux__
#include "epoll.hpp"

namespace NNet {

TEPoll::TEPoll()
    : Fd_(epoll_create1(EPOLL_CLOEXEC))
{
    if (Fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

TEPoll::~TEPoll()
{
    if (Fd_ >= 0) { close(Fd_); }
}

void TEPoll::Poll() {
    auto ts = GetTimeout();

    if (static_cast<int>(InEvents_.size()) <= MaxFd_) {
        InEvents_.resize(MaxFd_+1);
    }

    for (const auto& ch : Changes_) {
        int fd = ch.Fd;
        auto& ev  = InEvents_[fd];
        epoll_event eev = {};
        eev.data.fd = fd;
        bool change = false;
        bool newEv = false;
        if (ch.Handle) {
            newEv = !!ev.Read && !!ev.Write && !!ev.RHup;
            if (ch.Type & TEvent::READ) {
                eev.events |= EPOLLIN;
                change |= ev.Read != ch.Handle;
                ev.Read = ch.Handle;
            }
            if (ch.Type & TEvent::WRITE) {
                eev.events |= EPOLLOUT;
                change |= ev.Write != ch.Handle;
                ev.Write = ch.Handle;
            }
            if (ch.Type & TEvent::RHUP) {
                eev.events |= EPOLLRDHUP;
                change |= ev.RHup != ch.Handle;
                ev.RHup = ch.Handle;
            }
        } else {
            if (ch.Type & TEvent::READ) {
                change |= !!ev.Read;
                ev.Read = {};
            }
            if (ch.Type & TEvent::WRITE) {
                change |= !!ev.Write;
                ev.Write = {};
            }
            if (ev.Read) {
                eev.events |= EPOLLIN;
            }
            if (ev.Write) {
                eev.events |= EPOLLOUT;
            }
            if (ev.RHup) {
                eev.events |= EPOLLRDHUP;
            }
        }

        if (newEv) {
            if (epoll_ctl(Fd_, EPOLL_CTL_ADD, fd, &eev) < 0) {
                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
            }
        } else if (!eev.events) {
            if (epoll_ctl(Fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
                if (!(errno == EBADF || errno == ENOENT)) { // closed descriptor after TSocket -> close
                    throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                }
            }
        } else if (change) {
            if (epoll_ctl(Fd_, EPOLL_CTL_MOD, fd, &eev) < 0) {
                if (errno == ENOENT) {
                    if (epoll_ctl(Fd_, EPOLL_CTL_ADD, fd, &eev) < 0) {
                        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                    }
                } else {
                    throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                }
            }
        }
    }

    Reset();

    OutEvents_.resize(std::max<size_t>(1, InEvents_.size()));

    int nfds;
    if ((nfds =  epoll_pwait2(Fd_, &OutEvents_[0], OutEvents_.size(), &ts, nullptr)) < 0) {
        if (errno == EINTR) {
            return;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }

    for (int i = 0; i < nfds; ++i) {
        int fd = OutEvents_[i].data.fd;
        auto ev = InEvents_[fd];
        if (OutEvents_[i].events & EPOLLIN) {
            ReadyEvents_.emplace_back(TEvent{fd, TEvent::READ, ev.Read});
            ev.Read = {};
        }
        if (OutEvents_[i].events & EPOLLOUT) {
            ReadyEvents_.emplace_back(TEvent{fd, TEvent::WRITE, ev.Write});
            ev.Write = {};
        }
        if (OutEvents_[i].events & EPOLLHUP) {
            if (ev.Read) {
                ReadyEvents_.emplace_back(TEvent{fd, TEvent::READ, ev.Read});
            }
            if (ev.Write) {
                ReadyEvents_.emplace_back(TEvent{fd, TEvent::WRITE, ev.Write});
            }
        }
        if (OutEvents_[i].events & EPOLLRDHUP) {
            if (ev.Read) {
                ReadyEvents_.emplace_back(TEvent{fd, TEvent::READ, ev.Read});
            }
            if (ev.Write) {
                ReadyEvents_.emplace_back(TEvent{fd, TEvent::WRITE, ev.Write});
            }
            if (ev.RHup) {
                ReadyEvents_.emplace_back(TEvent{fd, TEvent::RHUP, ev.RHup});
            }
        }
    }

    ProcessTimers();
}

} // namespace NNet

#endif // __linux__
