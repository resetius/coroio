#pragma once

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include "poller.hpp"
#include "socket.hpp"

namespace NNet {

/**
 * @class TKqueue
 * @brief Poller implementation using kqueue (for macOS and FreeBSD).
 *
 * TKqueue inherits from TPollerBase and provides asynchronous I/O event polling based on
 * the kqueue API available on BSD-based systems (including macOS and FreeBSD). It manages
 * events through an internal list of kevent structures.
 *
 * The following type aliases are defined for convenience:
 *  - @c TSocket is defined as NNet::TSocket.
 *  - @c TFileHandle is defined as NNet::TFileHandle.
 *
 * Main methods:
 *  - The constructor and destructor (@ref TKqueue() and @ref ~TKqueue()) handle initialization
 *    and cleanup of the kqueue descriptor.
 *  - @ref Poll() waits for I/O events using kqueue, processes the changes, and updates internal
 *    event lists.
 *
 * Internal data members include:
 *  - @c Fd_: The kqueue file descriptor.
 *  - @c InEvents_: A container of all registered events.
 *  - @c ChangeList_: A vector storing modifications (kevent changes) to be applied.
 *  - @c OutEvents_: A vector to collect events returned by kevent().
 */
class TKqueue: public TPollerBase {
public:
    /// Alias for the socket type.
    using TSocket = NNet::TSocket;
    /// Alias for the file handle type.
    using TFileHandle = NNet::TFileHandle;
    /**
     * @brief Constructs the TKqueue instance.
     *
     * Initializes the kqueue file descriptor and internal event lists.
     */
    TKqueue();
    /**
     * @brief Destroys the TKqueue instance.
     *
     * Closes the kqueue descriptor and releases associated resources.
     */
    ~TKqueue();
    /**
     * @brief Polls for I/O events using kqueue.
     *
     * This method calls kevent() to wait for events, processes the resulting events from
     * @c OutEvents_, and updates internal structures accordingly.
     */
    void Poll();

private:
    int Fd_; ///< The kqueue file descriptor.
    std::vector<THandlePair> InEvents_; ///< All registered events in kqueue.
    std::vector<struct kevent> ChangeList_; ///< List of changes (kevent modifications).
    std::vector<struct kevent> OutEvents_; ///< Events returned from kevent().
};

} // namespace NNet
