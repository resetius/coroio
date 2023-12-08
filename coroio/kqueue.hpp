#pragma once

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TKqueue: public TPollerBase {
public:
    using TSocket = NNet::TSocket;
    using TFileHandle = NNet::TFileHandle;

    TKqueue();
    ~TKqueue();

    void Poll();

private:
    int Fd_;
    std::vector<THandlePair> InEvents_; // all events in kqueue
    std::vector<struct kevent> ChangeList_;
    std::vector<struct kevent> OutEvents_;
};

} // namespace NNet
