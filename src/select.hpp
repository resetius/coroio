#pragma once

#include <sys/select.h>
#include <assert.h>
#include <system_error>

#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TSelect: public TPollerBase {
public:
    using TSocket = NNet::TSocket;

    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        auto tv = GetTimeval(TClock::now(), deadline, MinDuration_);

        constexpr int bits = sizeof(fd_mask)*8;

        if (InEvents_.size() <= MaxFd_) {
            InEvents_.resize(MaxFd_+1);
        }
        if (MaxFd_ >= ReadFds_.size()*bits) {
            ReadFds_.resize((MaxFd_+bits)/bits);
            WriteFds_.resize((MaxFd_+bits)/bits);
        }

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

        if (select(InEvents_.size(), ReadFds(), WriteFds(), nullptr, &tv) < 0) {
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

private:
    fd_set* ReadFds() {
        return reinterpret_cast<fd_set*>(&ReadFds_[0]);
    }
    fd_set* WriteFds() {
        return reinterpret_cast<fd_set*>(&WriteFds_[0]);
    }

    std::vector<THandlePair> InEvents_;
    std::vector<fd_mask> ReadFds_;
    std::vector<fd_mask> WriteFds_;
};

} // namespace NNet
