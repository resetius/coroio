#if defined(__linux__) || defined(_WIN32)
#include "epoll.hpp"

namespace NNet {

namespace {
#ifdef _WIN32
int epoll_pwait2(HANDLE ephnd, struct epoll_event* events, int maxevents, const struct timespec* ts, const void* /*sigmask*/) {
    int timeout = 0;
    if (ts) {
        timeout  = ts->tv_sec;
        timeout += ts->tv_nsec / 1000000;
    }
    // TODO: support sigmask
    return epoll_wait(ephnd, events, maxevents, timeout);
}
#endif

uint64_t Pack(int fd, uint32_t generation) {
    return (static_cast<uint64_t>(generation) << 32) | static_cast<uint32_t>(fd);
}
}

#ifdef __linux__
static constexpr int epoll_flags = EPOLL_CLOEXEC;
static constexpr int invalid_handle = -1;
#else
static constexpr int epoll_flags = 0;
static constexpr HANDLE invalid_handle = nullptr;
#endif

TEPoll::TEPoll()
    : Fd_(epoll_create1(epoll_flags))
{
    if (Fd_ ==  invalid_handle) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
    SupportsRemoteHup_ = true;
}

TEPoll::~TEPoll()
{
    if (Fd_ != invalid_handle) {
#ifdef __linux__
        close(Fd_);
#endif
#ifdef _WIN32
        epoll_close(Fd_);
#endif
    }
}

void TEPoll::Poll() {
    auto ts = GetTimeout();

    if (static_cast<int>(InEvents_.size()) <= MaxFd_) {
        InEvents_.resize(MaxFd_+1);
        Generations_.resize(MaxFd_+1);
    }

    for (const auto& pending : Changes_) {
        int fd = pending.Fd;
        auto& ev  = InEvents_[fd];
        const auto ch = ResolveCancel(pending, ev);
        if (!ch.Type) {
            continue;
        }
        epoll_event eev = {};
        bool change = false;
        bool newEv = false;
        if (ch.Handle) {
            newEv = ev.Empty();
            if (ch.Type & TEvent::READ) {
                change |= ev.Read != ch.Handle;
                ev.Read = ch.Handle;
            }
            if (ch.Type & TEvent::WRITE) {
                change |= ev.Write != ch.Handle;
                ev.Write = ch.Handle;
            }
            if (ch.Type & TEvent::RHUP) {
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
            if (ch.Type & TEvent::RHUP) {
                change |= !!ev.RHup;
                ev.RHup = {};
            }
        }
        // EPOLL_CTL_MOD replaces the whole mask: keep the other waiters of the descriptor.
        if (ev.Read) {
            eev.events |= EPOLLIN;
        }
        if (ev.Write) {
            eev.events |= EPOLLOUT;
        }
        if (ev.RHup) {
            eev.events |= EPOLLRDHUP;
        }

        if (newEv) {
            eev.data.u64 = Pack(fd, ++Generations_[fd]);
            if (epoll_ctl(Fd_, EPOLL_CTL_ADD, fd, &eev) < 0) {
                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
            }
        } else if (!eev.events) {
            ev.Reset();
            if (epoll_ctl(Fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
                if (!(errno == EBADF || errno == ENOENT)) { // closed descriptor after TSocket -> close
                    throw std::system_error(errno, std::generic_category(), "epoll_ctl");
                }
            }
        } else if (change) {
            eev.data.u64 = Pack(fd, Generations_[fd]);
            if (epoll_ctl(Fd_, EPOLL_CTL_MOD, fd, &eev) < 0) {
                if (errno == ENOENT) {
                    eev.data.u64 = Pack(fd, ++Generations_[fd]);
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
        int fd = static_cast<uint32_t>(OutEvents_[i].data.u64);
        if (OutEvents_[i].data.u64 != Pack(fd, Generations_[fd])) {
            // Left by a closed descriptor whose file is still open elsewhere
            // (e.g. inherited by a child): epoll_ctl can no longer remove it.
            continue;
        }
        auto ev = InEvents_[fd];
        if ((OutEvents_[i].events & EPOLLIN) && ev.Read) {
            ReadyEvents_.emplace_back(TEvent{fd, TEvent::READ, ev.Read});
            ev.Read = {};
        }
        if ((OutEvents_[i].events & EPOLLOUT) && ev.Write) {
            ReadyEvents_.emplace_back(TEvent{fd, TEvent::WRITE, ev.Write});
            ev.Write = {};
        }
        // Errors and hang-ups, often reported together, wake every waiter once: an
        // error that wakes nobody would be reported again on every call.
        if (OutEvents_[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
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
