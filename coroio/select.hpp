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

#ifdef _WIN32
    TSelect();
#endif

private:
    fd_set* ReadFds() {
#ifdef _WIN32
        return &ReadFds_;
#else
        return reinterpret_cast<fd_set*>(&ReadFds_[0]);
#endif
    }
    fd_set* WriteFds() {
#ifdef _WIN32
        return &WriteFds_;
#else
        return reinterpret_cast<fd_set*>(&WriteFds_[0]);
#endif
    }

    std::vector<THandlePair> InEvents_;
#ifdef _WIN32
    fd_set ReadFds_;
    fd_set WriteFds_;
#else
    std::vector<fd_mask> ReadFds_;
    std::vector<fd_mask> WriteFds_;
#endif
};

} // namespace NNet
