#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif
#include <assert.h>
#include <system_error>

#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

class TSelect: public TPollerBase {
public:
    using TSocket = NNet::TSocket;
    using TFileHandle = NNet::TFileHandle;

    void Poll();

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
