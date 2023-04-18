#pragma once

#include <sys/select.h>

#include "poller.hpp"

namespace NNet {

class TSelect: public TPollerBase {
public:
    TSelect() { 

    }

    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        auto tv = GetTimeval(TClock::now(), deadline);
        int maxFd = -1;

        constexpr int bits = sizeof(fd_mask)*8;

        for (auto& [k, ev] : Events_) {
            if (k >= ReadFds_.size()*bits) {
                ReadFds_.resize((k+bits-1)/bits);
                WriteFds_.resize((k+bits-1)/bits);
            }
            if (InEvents_.size() <= k) {
                InEvents_.resize(k+1);
            }

            if (ev.Read) {
                FD_SET(k, ReadFds());
                InEvents_[k].Read = ev.Read;
            }
            if (ev.Write) {
                FD_SET(k, WriteFds());
                InEvents_[k].Write = ev.Write;
            }
            maxFd = std::max(maxFd, k);
        }

        Events_.clear();
        ReadyHandles_.clear();

        if (select(maxFd+1, ReadFds(), WriteFds(), nullptr, &tv) < 0) {
            throw std::system_error(errno, std::generic_category(), "select");
        }

        for (int k=0; k <= maxFd; ++k) {
            auto& ev = InEvents_[k];

            if (FD_ISSET(k, WriteFds())) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
                FD_CLR(k, WriteFds());
            }
            if (FD_ISSET(k, ReadFds())) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
                FD_CLR(k, ReadFds());
            }

            if (ev.Read || ev.Write) {
                Events_.emplace(k, ev);
            }
        }

        ProcessTimers();
    }

private:
    fd_set* ReadFds() {
        return reinterpret_cast<fd_set*>(&ReadFds_[0]);
    }
    fd_set* WriteFds() {
        return reinterpret_cast<fd_set*>(&WriteFds_[0]);
    }

    std::vector<TEvent> InEvents_;
    std::vector<fd_mask> ReadFds_;
    std::vector<fd_mask> WriteFds_;
};

} // namespace NNet
