#ifndef _WIN32
#include "poll.hpp"

namespace {

#if defined(__APPLE__) || defined(__FreeBSD__)
#define POLLRDHUP POLLHUP
int ppoll(struct pollfd* fds, nfds_t nfds, const struct timespec* ts, const sigset_t* /*sigmask*/) {
    int timeout = 0;
    if (ts) {
        timeout  = ts->tv_sec;
        timeout += ts->tv_nsec / 1000000;
    }
    // TODO: support sigmask
    return poll(fds, nfds, timeout);
}
#endif

} // namespace

namespace NNet {

void TPoll::Poll() {
    auto ts = GetTimeout();

    if (static_cast<int>(InEvents_.size()) <= MaxFd_) {
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
            if (ch.Type & TEvent::RHUP) {
                pev.events |= POLLRDHUP;
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
            if (ch.Type & TEvent::RHUP) {
                Fds_[idx].events &= ~POLLRDHUP;
                ev.RHup = {};
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
        if (pev.revents & POLLRDHUP) {
            if (ev.Read) {
                ReadyEvents_.emplace_back(TEvent{pev.fd, TEvent::READ, ev.Read});
            }
            if (ev.Write) {
                ReadyEvents_.emplace_back(TEvent{pev.fd, TEvent::WRITE, ev.Write});
            }
            if (ev.RHup) {
                ReadyEvents_.emplace_back(TEvent{pev.fd, TEvent::RHUP, ev.RHup});
            }
        }
    }

    ProcessTimers();
}

} // namespace NNet
#endif
