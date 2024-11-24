#pragma once

#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include "wepoll.h"
#endif

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
#ifdef __linux__
    int Fd_;
#endif

#ifdef _WIN32
    HANDLE Fd_;
#endif

    std::vector<THandlePair> InEvents_;       // all events in epoll
    std::vector<epoll_event> OutEvents_; // events out from epoll_wait
};

} // namespace NNet
