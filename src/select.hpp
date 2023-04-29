#pragma once

#include <sys/select.h>
#include <system_error>

#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TSelect: public TPollerBase {
public:
    using TSocket = NNet::TSocket;

    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        auto tv = GetTimeval(TClock::now(), deadline);

        constexpr int bits = sizeof(fd_mask)*8;

        if (InEvents_.size() <= MaxFd_) {
            InEvents_.resize(MaxFd_+1);
        }
        if (MaxFd_ >= ReadFds_.size()*bits) {
            ReadFds_.resize((MaxFd_+bits)/bits);
            WriteFds_.resize((MaxFd_+bits)/bits);
        }
        while (!DelReads_.empty()) {
            auto k = DelReads_.front(); DelReads_.pop();
            FD_CLR(k, ReadFds()); InEvents_[k].Read = {};
        }
        while(!DelWrites_.empty()) {
            auto k = DelWrites_.front(); DelWrites_.pop();
            FD_CLR(k, WriteFds()); InEvents_[k].Write = {};
        }
        for (auto [k, h] : NewReads_) {
            FD_SET(k, ReadFds()); InEvents_[k].Read = h;
        }
        for (auto [k, h] : NewWrites_) {
            FD_SET(k, WriteFds()); InEvents_[k].Write = h;
        }

        Reset();

        if (select(InEvents_.size(), ReadFds(), WriteFds(), nullptr, &tv) < 0) {
            throw std::system_error(errno, std::generic_category(), "select");
        }

        for (size_t k=0; k < InEvents_.size(); ++k) {
            auto ev = InEvents_[k];

            if (FD_ISSET(k, WriteFds())) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                DelWrites_.emplace(k);
            } else if (ev.Write) {
                // fd was cleared by select, set it
                FD_SET(k, WriteFds());
            }
            if (FD_ISSET(k, ReadFds())) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                DelReads_.emplace(k);
            } else if (ev.Read) {
                // fd was cleared by select, set it
                FD_SET(k, ReadFds());
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
