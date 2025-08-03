#pragma once

#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <coroio/backends/wepoll.h>
#endif

#include <coroio/base.hpp>
#include <coroio/poller.hpp>
#include <coroio/socket.hpp>

namespace NNet {

/**
 * @class TEPoll
 * @brief Linux-specific poller implementation using epoll.
 *
 * TEPoll inherits from TPollerBase and provides an implementation of asynchronous
 * I/O event polling based on the Linux epoll API. It is used only on Linux systems.
 *
 * The class defines the following type aliases for ease of use:
 *  - @c TSocket is defined as NNet::TSocket.
 *  - @c TFileHandle is defined as NNet::TFileHandle.
 *
 * Main methods:
 *  - @ref TEPoll() and @ref ~TEPoll() for construction and cleanup.
 *  - @ref Poll() which polls for I/O events and processes them.
 *
 * Internal data members include:
 *  - The epoll file descriptor (@c Fd_).
 *  - An internal container (@c InEvents_) holding all registered events.
 *  - A vector (@c OutEvents_) to store the events returned by epoll_wait.
 *
 * @note This class is only supported on Linux.
 */
class TEPoll: public TPollerBase {
public:
    /// Alias for the socket type.
    using TSocket = NNet::TSocket;
    /// Alias for the file handle type.
    using TFileHandle = NNet::TFileHandle;

    /**
     * @brief Constructs the TEPoll instance.
     */
    TEPoll();
    /**
     * @brief Destructs the TEPoll instance and cleans up resources.
     */
    ~TEPoll();
    /**
     * @brief Polls for I/O events using the epoll API.
     *
     * This method calls epoll_wait to detect ready events, processes the returned events,
     * and updates the internal event structures.
     */
    void Poll();

private:
#ifdef __linux__
    int Fd_; ///< The epoll file descriptor (used only on Linux).
#endif

#ifdef _WIN32
    HANDLE Fd_; ///< (Not used on Linux).
#endif

    std::vector<THandlePair> InEvents_;  ///< All registered events.
    std::vector<epoll_event> OutEvents_; ///< Events returned from epoll_wait.
};

} // namespace NNet
