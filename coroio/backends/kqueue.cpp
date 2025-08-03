#if defined(__APPLE__) || defined(__FreeBSD__)
#include <system_error>
#include "kqueue.hpp"

namespace NNet {

TKqueue::TKqueue()
    : Fd_(kqueue())
{
    if (Fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "kqueue");
    }
}

TKqueue::~TKqueue()
{
    if (Fd_ >= 0) { close(Fd_); }
}

void TKqueue::Poll()
{
    auto ts = GetTimeout();

    ChangeList_.clear();
    if (static_cast<int>(InEvents_.size()) <= MaxFd_) {
        InEvents_.resize(MaxFd_+1);
    }
    for (auto& ch : Changes_) {
        int fd = ch.Fd;
        struct kevent kev = {};
        auto& ev = InEvents_[fd];

        if (ch.Handle) {
            if (ch.Type & TEvent::READ && ev.Read != ch.Handle) {
                EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                ev.Read = ch.Handle;
            }
            if (ch.Type & TEvent::WRITE && ev.Write != ch.Handle) {
                EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                ev.Write = ch.Handle;
            }
        } else {
            if (ch.Type & TEvent::READ && ev.Read) {
                EV_SET(&kev, fd, EVFILT_READ, EV_DELETE | EV_CLEAR, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                ev.Read = {};
            }
            if (ch.Type & TEvent::WRITE && ev.Write) {
                EV_SET(&kev, fd, EVFILT_WRITE, EV_DELETE | EV_CLEAR, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                ev.Write = {};
            }
        }
    }

    Reset();

    OutEvents_.resize(std::max<size_t>(1, 2*InEvents_.size()));
    int nfds;
    if ((nfds = kevent(
                Fd_,
                ChangeList_.data(), ChangeList_.size(),
                OutEvents_.data(), OutEvents_.size(),
                &ts)) < 0)
    {
        throw std::system_error(errno, std::generic_category(), "kevent");
    }
    for (int i = 0; i < nfds; i++) {
        int fd = OutEvents_[i].ident;
        int filter = OutEvents_[i].filter;
        int flags = OutEvents_[i].flags;
        if (flags & EV_DELETE) {
            // closed socket?
            continue;
        }
        // TODO: check flags & EV_ERROR && errno = OutEvents_[i].data
        THandlePair ev = InEvents_[fd];
        if (filter == EVFILT_READ && ev.Read) {
            ReadyEvents_.emplace_back(TEvent{fd, TEvent::READ, ev.Read});
            ev.Read = {};
        }
        if (filter == EVFILT_WRITE && ev.Write) {
            ReadyEvents_.emplace_back(TEvent{fd, TEvent::WRITE, ev.Write});
            ev.Write = {};
        }
        if (flags & EV_EOF) {
            if (ev.Read) {
                ReadyEvents_.emplace_back(TEvent{fd, TEvent::READ, ev.Read});
            }
            if (ev.Write) {
                ReadyEvents_.emplace_back(TEvent{fd, TEvent::WRITE, ev.Write});
            }
        }
    }

    ProcessTimers();
}

} // namespace NNet

#endif // #if defined(__APPLE__) || defined(__FreeBSD__)
