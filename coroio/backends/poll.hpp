#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

#include <assert.h>

#include <iostream>

#include <coroio/base.hpp>
#include <coroio/poller.hpp>
#include <coroio/socket.hpp>

namespace NNet {

/**
 * @class TPoll
 * @brief Poller implementation based on the poll() system call.
 *
 * TPoll inherits from TPollerBase and implements asynchronous I/O event polling using
 * the poll() system call. It manages events via a vector of pollfd structures and an
 * internal list of event registrations (stored as tuples pairing THandlePair with an index).
 *
 * Platform-specific notes:
 * - On Windows, a dummy socket (@c DummySocket_) is maintained to properly handle timeouts.
 *
 * This class provides type aliases for the socket and file handle types:
 *  - @c TSocket is defined as NNet::TSocket.
 *  - @c TFileHandle is defined as NNet::TFileHandle.
 *
 * The main method is:
 *  - @ref Poll(), which performs the actual polling and processes incoming events.
 *
 * @see TPollerBase, TSelect.
 */
class TPoll: public TPollerBase {
public:
    /// Alias for the socket type.
    using TSocket = NNet::TSocket;
    /// Alias for the file handle type.
    using TFileHandle = NNet::TFileHandle;

#ifdef _WIN32
    /**
     * @brief Default constructor for Windows.
     *
     * On Windows, additional initialization (such as setting up a dummy socket)
     * is performed.
     */
    TPoll();
#endif

    /**
     * @brief Polls for I/O events.
     *
     * This method uses the poll() system call to wait for registered events on file
     * descriptors. It processes incoming events by matching them against the internal list
     * of changes and then waking up any suspended coroutines waiting on those events.
     */
    void Poll();

private:
    /**
     * @brief Internal container for registered events.
     *
     * Each element is a tuple consisting of a THandlePair (an event) and an integer index
     * representing its position in the Fds_ vector.
     */
    std::vector<std::tuple<THandlePair,int>> InEvents_;
    /**
     * @brief The pollfd vector used by poll().
     *
     * This vector holds the file descriptors and event masks used in the poll() call.
     */
    std::vector<pollfd> Fds_;
#ifdef _WIN32
    /**
     * @brief Dummy socket used on Windows.
     *
     * On Windows, a dummy socket is maintained to properly handle timeouts.
     */
    TSocket DummySocket_;
#endif
};

} // namespace NNet
