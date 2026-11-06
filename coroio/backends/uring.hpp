#pragma once

#if __has_include(<liburing.h>)
#define HAVE_URING

#include <coroio/base.hpp>
#include <coroio/completions.hpp>
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
 * - Requires Linux 6.1 or later. The ring is set up with IORING_SETUP_SINGLE_ISSUER, so it must be
 *   used from the thread that constructs it.
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
 * // Post an asynchronous read. op must stay in place until it is delivered or cancelled.
 * TUring::TOperation op;
 * op.Handle = coroutineHandle;
 * uringPoller.Read(socketFd, buffer, bufferSize, op);
 *
 * // Poll for I/O completions.
 * int ret = uringPoller.Wait();
 *
 * // Resume the coroutines of the completed operations. The read's result is in op.Result.
 * uringPoller.WakeupReadyHandles();
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
     * @brief State of one operation, kept by the caller.
     *
     * Set TOperation::Handle to the coroutine to resume before posting the
     * operation; if posting throws, it is reset. The address of the state is
     * the operation's user_data, so it must stay in place until
     * WakeupReadyHandles() delivers the completion or Cancel() returns.
     * The result of the completion is in TOperation::Result.
     */
    using TOperation = NDetail::TOperationState;
    /**
     * @brief Constructs a TUring instance.
     *
     * @param queueSize The desired size of the io_uring submission queue (default is 256).
     *                  At least 2: a close takes two linked entries.
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
     * @param op State of the operation, with the coroutine to resume upon completion.
     */
    void Read(int fd, void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous write operation.
     *
     * @param fd The file descriptor.
     * @param buf Buffer with data to write.
     * @param size Number of bytes to write.
     * @param op State of the operation, with the coroutine to resume upon completion.
     */
    void Write(int fd, const void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous receive operation.
     *
     * @param fd The file descriptor.
     * @param buf Buffer where received data will be stored.
     * @param size Maximum number of bytes to receive.
     * @param op State of the operation, with the coroutine to resume upon completion.
     */
    void Recv(int fd, void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous send operation.
     *
     * @param fd The file descriptor.
     * @param buf Buffer containing data to send.
     * @param size Number of bytes to send.
     * @param op State of the operation, with the coroutine to resume upon completion.
     */
    void Send(int fd, const void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous accept operation.
     *
     * @param fd The listening socket descriptor.
     * @param addr Pointer to a sockaddr structure where the client address is stored.
     * @param len Pointer to the length of the address structure.
     * @param op State of the operation, with the coroutine to resume upon acceptance.
     */
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, TOperation& op);
    /**
     * @brief Posts an asynchronous connect operation.
     *
     * @param fd The socket descriptor.
     * @param addr Pointer to the destination address.
     * @param len Size of the destination address structure.
     * @param op State of the operation, with the coroutine to resume upon connection.
     */
    void Connect(int fd, const sockaddr* addr, socklen_t len, TOperation& op);
    /**
     * @brief Cancels pending operations on the specified file descriptor.
     *
     * @param fd The file descriptor.
     */
    void Cancel(int fd);
    /**
     * @brief Cancels a pending operation.
     *
     * Called when the coroutine is destroyed while waiting for the operation.
     * An operation not submitted yet is dropped. Otherwise it is cancelled, and
     * the call returns only after the kernel has finished with it, so its
     * buffers and @p op may be freed. Either way its completion is dropped, so
     * its coroutine is not resumed. An operation that completed before the
     * cancellation loses its result, e.g. the data it read.
     *
     * @param op State of the operation, not delivered yet.
     * @return The result of the dropped completion, e.g. a descriptor accepted meanwhile,
     *         or a negative value.
     */
    int Cancel(TOperation& op);
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
     * @brief Resumes the coroutines whose operations completed.
     *
     * Skips completions dropped by Cancel(TOperation&). A completion collected
     * by Wait() and not delivered here is kept by the next Wait().
     */
    void WakeupReadyHandles();

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
    /// Obtains an SQE for @p op on @p fd. If that throws, op.Handle is reset: the operation is not posted.
    io_uring_sqe* GetSqe(TOperation& op, int fd) {
        try {
            if (fd >= static_cast<int>(Operations_.size())) {
                Operations_.resize(fd + 1); // here, so that SetData() cannot throw
            }
            return GetSqe();
        } catch (...) {
            op.Handle = {};
            throw;
        }
    }

    /// Submits queued I/O requests to the kernel.
    void Submit();
    /// Queues the close of a close change, hard-linked after a cancel by fd if the descriptor has operations. Not named Close: it would hide TPollerBase::Close.
    void PrepClose(TEvent& change);
    /// Points the SQE just obtained to @p op, records its position in the SQ and counts it for its descriptor.
    void SetData(io_uring_sqe* sqe, TOperation& op);
    /// Uncounts @p op, whose completion is taken or whose SQE is dropped.
    void Done(TOperation& op);
    /// Whether operations posted on @p fd may still be in the kernel.
    bool HasOperations(int fd) const;
    /// Reaps the posted completions, except the one of @p op, whose result goes to @p result.
    bool Reap(TOperation& op, int* result);

    int RingFd_; ///< File descriptor for the io_uring.
    int EpollFd_; ///< Epoll file descriptor (for integration with epoll).
    struct io_uring Ring_; ///< The io_uring structure.
    NDetail::TCompletions Completions_; ///< Completions not delivered yet.
    std::vector<unsigned> Operations_; ///< Per descriptor: operations posted whose completion is not taken yet.
    std::vector<char> Buffer_; ///< Buffer used for internal I/O operations.
};

} // namespace NNet

#endif
