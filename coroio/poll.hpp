#pragma once

#include <poll.h>
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

    void Poll();

private:
    std::vector<std::tuple<THandlePair,int>> InEvents_; // event + index in Fds
    std::vector<pollfd> Fds_;
};

} // namespace NNet
