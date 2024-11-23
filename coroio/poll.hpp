#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

#include <assert.h>

#include <iostream>

#include "base.hpp"
#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TPoll: public TPollerBase {
public:
    using TSocket = NNet::TSocket;
    using TFileHandle = NNet::TFileHandle;

#ifdef _WIN32
    TPoll();
#endif

    void Poll();

private:
    std::vector<std::tuple<THandlePair,int>> InEvents_; // event + index in Fds
    std::vector<pollfd> Fds_;
#ifdef _WIN32
    TSocket DummySocket_; // for timeouts
#endif
};

} // namespace NNet
