#if defined(__APPLE__) || defined(__FreeBSD__)
#include <system_error>
#include "kqueue.hpp"

namespace NNet {

namespace {

// EVFILT_READ serves both the reader and the RHUP waiter. Without a reader it
// is edge triggered: unread data would make a level triggered filter fire on
// every call.
int ReadFilterFlags(const THandlePair& ev) {
    return ev.Read ? EV_ADD : ev.RHup ? EV_ADD | EV_CLEAR : 0;
}

} // namespace

TKqueue::TKqueue()
    : Fd_(kqueue())
{
    if (Fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "kqueue");
    }
    SupportsRemoteHup_ = true;
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
    for (auto& pending : Changes_) {
        int fd = pending.Fd;
        struct kevent kev = {};
        auto& ev = InEvents_[fd];
        const auto ch = Resolve(pending, ev);
        if (!ch.Type) {
            continue;
        }
        const auto prev = ev;

        if (ch.Type & TEvent::CLOSE) {
            // close() drops the kevents; changes queued before it resolved to nothing.
            ev.Reset();
            CloseDescriptor(pending);
            continue;
        }

        if (ch.Handle) {
            if (ch.Type & TEvent::READ) {
                ev.Read = ch.Handle;
            }
            if (ch.Type & TEvent::WRITE && ev.Write != ch.Handle) {
                EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                ev.Write = ch.Handle;
            }
            if (ch.Type & TEvent::RHUP) {
                ev.RHup = ch.Handle;
            }
        } else {
            if (ch.Type & TEvent::READ) {
                ev.Read = {};
            }
            if (ch.Type & TEvent::WRITE && ev.Write) {
                EV_SET(&kev, fd, EVFILT_WRITE, EV_DELETE | EV_CLEAR, 0, 0, nullptr);
                ChangeList_.emplace_back(kev);
                ev.Write = {};
            }
            if (ch.Type & TEvent::RHUP) {
                ev.RHup = {};
            }
        }

        const int prevFlags = ReadFilterFlags(prev);
        const int flags = ReadFilterFlags(ev);
        if (prevFlags && prevFlags != flags) {
            EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            ChangeList_.emplace_back(kev);
        }
        // Adding an existing filter again rearms it, so a new RHUP waiter also
        // sees an EOF that has already been reported.
        const bool rearm = ev.Read != prev.Read || (ch.Handle && (ch.Type & TEvent::RHUP));
        if (flags && (flags != prevFlags || rearm)) {
            EV_SET(&kev, fd, EVFILT_READ, flags, 0, 0, nullptr);
            ChangeList_.emplace_back(kev);
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
        if (filter == EVFILT_READ && (flags & EV_EOF) && ev.RHup) {
            ReadyEvents_.emplace_back(TEvent{fd, TEvent::RHUP, ev.RHup});
        }
        // EOF is reported by each filter, so it wakes only the waiters of that
        // filter: waking the others here would resume them twice.
    }

    ProcessTimers();
}

} // namespace NNet

#endif // #if defined(__APPLE__) || defined(__FreeBSD__)
