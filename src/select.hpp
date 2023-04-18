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

        constexpr int bits = sizeof(fd_mask)*8;

        for (auto& [k, ev] : Events_) {
            if (k >= ReadFds_.size()*bits) {
                ReadFds_.resize((k+bits-1)/bits);
                WriteFds_.resize((k+bits-1)/bits);
            }
            if (InEvents_.size() <= k) {
                InEvents_.resize(k+1);
            }
            auto& old_ev = InEvents_[k];

            if (ev.Read&&!old_ev.Read) {
                FD_SET(k, ReadFds());
                old_ev.Read = ev.Read;
            }
            if (ev.Write&&!old_ev.Write) {
                FD_SET(k, WriteFds());
                old_ev.Write = ev.Write;
            }
            if (!ev.Read&&old_ev.Read) {
                FD_CLR(k, ReadFds());
                old_ev.Read = ev.Read;
            }
            if (!ev.Write&&old_ev.Write) {
                FD_CLR(k, WriteFds());
                old_ev.Write = ev.Write;
            }

            while (!InEvents_.empty() && !InEvents_.back().Write && !InEvents_.back().Read) {
                InEvents_.pop_back();
            }
        }

        Events_.clear();
        ReadyHandles_.clear();

        if (select(InEvents_.size(), ReadFds(), WriteFds(), nullptr, &tv) < 0) {
            throw std::system_error(errno, std::generic_category(), "select");
        }

        for (size_t k=0; k < InEvents_.size(); ++k) {
            auto ev = InEvents_[k];
            bool changed = false;

            if (FD_ISSET(k, WriteFds())) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
                ev.Write = {};
                changed |= true;
            } else if (ev.Write) {
                // fd was cleared by select, set it
                FD_SET(k, WriteFds());
            }
            if (FD_ISSET(k, ReadFds())) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
                ev.Read = {};
                changed |= true;
            } else if (ev.Read) {
                // fd was cleared by select, set it
                FD_SET(k, ReadFds());
            }

            if (changed) {
                // keep changes small
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
