#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include "base.hpp"
#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TEPoll: public TPollerBase {
public:
    using TSocket = NNet::TSocket;
    using TFileHandle = NNet::TFileHandle;

    TEPoll();
    ~TEPoll();

    void Poll();

private:
    int Fd_;
    std::vector<THandlePair> InEvents_;       // all events in epoll
    std::vector<epoll_event> OutEvents_; // events out from epoll_wait
};

} // namespace NNet
