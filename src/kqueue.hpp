#pragma once

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <system_error>

#include "poller.hpp"

namespace NNet {

class TKqueue: public TPollerBase {
public:
    TKqueue()
        : Fd_(kqueue())
    {
        if (Fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "kqueue");
        }
    }

    ~TKqueue()
    {
        if (Fd_ >= 0) { close(Fd_); }
    }

    void Poll()
    {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        auto ts = GetTimespec(TClock::now(), deadline);

        ChangeList_.clear();
        for (auto& [k, ev] : Events_) {
            struct kevent kev = {};
            bool changed = false;

            if (InEvents_.size() <= k) {
                InEvents_.resize(k+1);
            }

            auto& old_ev = InEvents_[k];

            if (ev.Read && !old_ev.Read) {
                EV_SET(&kev, k, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                old_ev.Read = ev.Read;
            }
            if (ev.Write && !old_ev.Write) {
                EV_SET(&kev, k, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                old_ev.Write = ev.Write;
            }
            if (!ev.Read && old_ev.Read) {
                EV_SET(&kev, k, EVFILT_READ, EV_DELETE | EV_CLEAR, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                old_ev.Read = ev.Read;
            }
            if (!ev.Write && old_ev.Write) {
                EV_SET(&kev, k, EVFILT_WRITE, EV_DELETE | EV_CLEAR, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                old_ev.Write = ev.Write;
            }
        }

        Events_.clear();
        OutEvents_.resize(std::max<size_t>(1, 2*InEvents_.size()));
        int nfds;
        if ((nfds = kevent(
                 Fd_,
                 &ChangeList_[0], ChangeList_.size(),
                 &OutEvents_[0], OutEvents_.size(),
                 &ts)) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "kevent");
        }

        ReadyHandles_.clear();

        for (int i = 0; i < nfds; i++) {
            int fd = OutEvents_[i].ident;
            int filter = OutEvents_[i].filter;
            int flags = OutEvents_[i].flags;
            if (flags & EV_DELETE) {
                // closed socket?
                continue;
            }
            // TODO: check flags & EV_ERROR && errno = OutEvents_[i].data
            auto maybeEv = Events_.find(fd);
            TEvent ev;
            if (maybeEv == Events_.end()) {
                ev = InEvents_[fd];
            } else {
                ev = maybeEv->second;
            }
            bool changed = false;
            if (filter == EVFILT_READ && ev.Read) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
                changed |= true;
            }
            if (filter == EVFILT_WRITE && ev.Write) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                assert(ev.Write);
                ev.Write = {};
                changed |= true;
            }
            if (flags & EV_EOF) {
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
                Events_[fd] = ev;
            }
        }

        ProcessTimers();
    }

private:
    int Fd_;
    std::vector<TEvent> InEvents_; // all events in kqueue
    std::vector<struct kevent> ChangeList_;
    std::vector<struct kevent> OutEvents_;
};

} // namespace NNet
