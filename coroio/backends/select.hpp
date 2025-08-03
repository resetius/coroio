#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif
#include <assert.h>
#include <system_error>

#include <coroio/poller.hpp>
#include <coroio/socket.hpp>

namespace NNet {

/**
 * @class TSelect
 * @brief Poller implementation based on the select() system call.
 *
 * TSelect inherits from TPollerBase and implements asynchronous I/O polling using
 * the select() system call. It manages file descriptors using fd_set structures (or
 * equivalent containers on non‑Windows systems) and provides helper functions for
 * obtaining pointers to the read and write fd_sets.
 *
 * On Windows, a dummy socket is maintained to handle timeouts correctly.
 *
 * The class also defines convenient type aliases:
 *  - @c TSocket is defined as NNet::TSocket.
 *  - @c TFileHandle is defined as NNet::TFileHandle.
 *
 * Main method:
 *  - @ref Poll() is called to perform the select() operation and process events.
 *
 * @see TPollerBase, NNet::TSocket, NNet::TFileHandle.
 */
class TSelect: public TPollerBase {
public:
    /// Alias for the socket type.
    using TSocket = NNet::TSocket;
    /// Alias for the file handle type.
    using TFileHandle = NNet::TFileHandle;
    /**
     * @brief Polls for I/O events using the select() system call.
     *
     * This method builds the file descriptor sets from the pending events, calls
     * select(), and then processes any triggered events.
     */
    void Poll();

#ifdef _WIN32
    /**
     * @brief Default constructor for Windows.
     *
     * On Windows, additional initialization is performed, such as preparing the dummy socket.
     */
    TSelect();
#endif

private:
    /**
     * @brief Returns a pointer to the read fd_set.
     *
     * On Windows, this returns the address of a native fd_set.
     * On Unix-like systems, it returns a pointer to the underlying data of the vector.
     *
     * @return A pointer to the read file descriptor set.
     */
    fd_set* ReadFds() {
#ifdef _WIN32
        return &ReadFds_;
#else
        return reinterpret_cast<fd_set*>(&ReadFds_[0]);
#endif
    }

    /**
     * @brief Returns a pointer to the write fd_set.
     *
     * On Windows, this returns the address of a native fd_set.
     * On Unix-like systems, it returns a pointer to the underlying data of the vector.
     *
     * @return A pointer to the write file descriptor set.
     */
    fd_set* WriteFds() {
#ifdef _WIN32
        return &WriteFds_;
#else
        return reinterpret_cast<fd_set*>(&WriteFds_[0]);
#endif
    }

    std::vector<THandlePair> InEvents_; ///< Internal container for incoming event pairs.
#ifdef _WIN32
    fd_set ReadFds_; ///< Native fd_set for reading on Windows.
    fd_set WriteFds_; ///< Native fd_set for wrinting on Windows.
    TSocket DummySocket_; ///< Dummy socket used for handling timeouts on Windows.
#else
    std::vector<fd_mask> ReadFds_; ///< Underlying buffer for read fd_set on Unix-like systems.
    std::vector<fd_mask> WriteFds_; ///< Underlying buffer for write fd_set on Unix-like systems.
#endif
};

} // namespace NNet
