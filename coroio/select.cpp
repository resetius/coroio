#include "select.hpp"

namespace NNet {

#ifdef _WIN32
    TSelect::TSelect() {
        FD_ZERO(&ReadFds_);
        FD_ZERO(&WriteFds_);
    }
#endif

void TSelect::Poll() {
    auto ts = GetTimeout();

    if (static_cast<int>(InEvents_.size()) <= MaxFd_) {
        InEvents_.resize(MaxFd_+1);
    }

#ifndef _WIN32
    constexpr int bits = sizeof(fd_mask)*8;

    if (MaxFd_ >= static_cast<int>(ReadFds_.size())*bits) {
        ReadFds_.resize((MaxFd_+bits)/bits);
        WriteFds_.resize((MaxFd_+bits)/bits);
    }
#endif

    for (const auto& ch : Changes_) {
        int fd = ch.Fd;
        auto& ev = InEvents_[fd];
        if (ch.Handle) {
            if (ch.Type & TEvent::READ) {
                FD_SET(fd, ReadFds()); ev.Read = ch.Handle;
            }
            if (ch.Type & TEvent::WRITE) {
                FD_SET(fd, WriteFds()); ev.Write = ch.Handle;
            }
        } else {
            if (ch.Type & TEvent::READ) {
                FD_CLR(fd, ReadFds()); ev.Read = {};
            }
            if (ch.Type & TEvent::WRITE) {
                FD_CLR(fd, WriteFds()); ev.Write = {};
            }
        }
    }

    Reset();

#ifdef _WIN32
    timeval tv;
    tv.tv_sec = ts.tv_sec;
    tv.tv_usec = ts.tv_nsec / 1000;

    if (select(InEvents_.size(), ReadFds(), WriteFds(), nullptr, &tv) < 0) {
#else
    if (pselect(InEvents_.size(), ReadFds(), WriteFds(), nullptr, &ts, nullptr) < 0) {
#endif
        throw std::system_error(errno, std::generic_category(), "select");
    }

    for (int k=0; k < static_cast<int>(InEvents_.size()); ++k) {
        auto ev = InEvents_[k];

        if (FD_ISSET(k, WriteFds())) {
            assert(ev.Write);
            ReadyEvents_.emplace_back(TEvent{k, TEvent::WRITE, ev.Write});
        } else if (ev.Write) {
            // fd was cleared by select, set it
            FD_SET(k, WriteFds());
        }
        if (FD_ISSET(k, ReadFds())) {
            assert(ev.Read);
            ReadyEvents_.emplace_back(TEvent{k, TEvent::READ, ev.Read});
        } else if (ev.Read) {
            // fd was cleared by select, set it
            FD_SET(k, ReadFds());
        }
    }

    ProcessTimers();
}

} // namespace NNet
