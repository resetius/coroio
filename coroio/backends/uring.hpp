#pragma once

#if __has_include(<liburing.h>) 
#define HAVE_URING

#include <coroio/base.hpp>
#include <coroio/socket.hpp>
#include <coroio/poller.hpp>

#include <liburing.h>
#include <assert.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <system_error>
#include <iostream>
#include <tuple>
#include <vector>
#include <coroutine>
#include <queue>

namespace NNet {

/**
 * @class TUring
 * @brief Poller implementation based on io_uring.
 *
 * TUring inherits from TPollerBase and implements asynchronous I/O operations using the Linux io_uring API.
 * It provides methods for performing asynchronous read, write, receive, send, accept, and connect operations.
 *
 * Key features:
 * - Uses io_uring to queue and submit asynchronous I/O operations.
 * - Provides operations such as @ref Read(), @ref Write(), @ref Recv(), @ref Send(), @ref Accept() and @ref Connect().
 * - Offers additional methods for cancelling pending operations, registering file descriptors, waiting for completions,
 *   and submitting queued requests.
 *
 * Type aliases:
 *  - @c TSocket is defined as NNet::TPollerDrivenSocket<TUring>.
 *  - @c TFileHandle is defined as NNet::TPollerDrivenFileHandle<TUring>.
 *
 * Example usage:
 * @code{.cpp}
 * // Create an io_uring poller with a queue size of 256.
 * TUring uringPoller(256);
 *
 * // Register a socket or file descriptor.
 * uringPoller.Register(socketFd);
 *
 * // Post an asynchronous read.
 * uringPoller.Read(socketFd, buffer, bufferSize, coroutineHandle);
 *
 * // Submit queued operations.
 * uringPoller.Submit();
 *
 * // Poll for I/O completions.
 * int ret = uringPoller.Wait();
 *
 * // Process the result of the completed operations.
 * int result = uringPoller.Result();
 * @endcode
 *
 */
class TUring: public TPollerBase {
public:
    /// Alias for the poller-driven socket type.
    using TSocket = NNet::TPollerDrivenSocket<TUring>;
    /// Alias for the poller-driven file handle type.
    using TFileHandle = NNet::TPollerDrivenFileHandle<TUring>;
    /**
     * @brief Constructs a TUring instance.
     *
     * @param queueSize The desired size of the io_uring submission queue (default is 256).
     */
    TUring(int queueSize = 256);
    /// Destructor cleans up the io_uring and related resources.
    ~TUring();
    /**
     * @brief Posts an asynchronous read operation.
     *
     * @param fd The file descriptor.
     * @param buf Buffer where data is to be stored.
     * @param size Number of bytes to read.
     * @param handle Coroutine handle to resume upon completion.
     */
    void Read(int fd, void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous write operation.
     *
     * @param fd The file descriptor.
     * @param buf Buffer with data to write.
     * @param size Number of bytes to write.
     * @param handle Coroutine handle to resume upon completion.
     */
    void Write(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous receive operation.
     *
     * @param fd The file descriptor.
     * @param buf Buffer where received data will be stored.
     * @param size Maximum number of bytes to receive.
     * @param handle Coroutine handle to resume upon completion.
     */
    void Recv(int fd, void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous send operation.
     *
     * @param fd The file descriptor.
     * @param buf Buffer containing data to send.
     * @param size Number of bytes to send.
     * @param handle Coroutine handle to resume upon completion.
     */
    void Send(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous accept operation.
     *
     * @param fd The listening socket descriptor.
     * @param addr Pointer to a sockaddr structure where the client address is stored.
     * @param len Pointer to the length of the address structure.
     * @param handle Coroutine handle to resume upon acceptance.
     */
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous connect operation.
     *
     * @param fd The socket descriptor.
     * @param addr Pointer to the destination address.
     * @param len Size of the destination address structure.
     * @param handle Coroutine handle to resume upon connection.
     */
    void Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle);
    /**
     * @brief Cancels pending operations on the specified file descriptor.
     *
     * @param fd The file descriptor.
     */
    void Cancel(int fd);
    /**
     * @brief Cancels pending operations associated with a specific coroutine handle.
     *
     * @param h The coroutine handle.
     */
    void Cancel(std::coroutine_handle<> h);
    /**
     * @brief Registers a file descriptor with the IO_uring poller.
     *
     * @param fd The file descriptor to register.
     */
    void Register(int fd);
   /**
     * @brief Waits for I/O completions.
     *
     * @param ts Optional timeout (default is 10 seconds, expressed as {10, 0}).
     * @return The number of completed events (or a negative value on error).
     */
    int Wait(timespec ts = {10,0});
    /**
     * @brief Polls for I/O completions.
     *
     * This method simply calls Wait() with a computed timeout.
     */
    void Poll() {
        Wait(GetTimeout());
    }
    /**
     * @brief Retrieves the result of the last completed I/O completion.
     *
     * @return An integer result, such as the number of bytes transferred.
     */
    int Result();
    /// Submits queued I/O requests to the kernel.
    void Submit();

private:
    /**
     * @brief Obtains an available submission queue entry (SQE) for io_uring.
     *
     * If no SQE is available, it automatically calls Submit() and retries.
     *
     * @return Pointer to an io_uring_sqe.
     */
    io_uring_sqe* GetSqe() {
        io_uring_sqe* r = io_uring_get_sqe(&Ring_);
        if (!r) {
            Submit();
            r = io_uring_get_sqe(&Ring_);
        }
        return r;
    }

    int RingFd_; ///< File descriptor for the io_uring.
    int EpollFd_; ///< Epoll file descriptor (for integration with epoll).
    struct io_uring Ring_; ///< The io_uring structure.
    std::queue<int> Results_; ///< Queue of results for completed operations.
    std::vector<char> Buffer_; ///< Buffer used for internal I/O operations.
};

} // namespace NNet

#endif
