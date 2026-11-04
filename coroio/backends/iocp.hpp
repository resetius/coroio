#pragma once

#include <coroio/base.hpp>
#include <coroio/socket.hpp>
#include <coroio/poller.hpp>
#include <coroio/arena.hpp>
#include <coroio/intrusive_list.hpp>

namespace NNet {

/**
 * @class TIOCp
 * @brief IOCP-based poller for asynchronous I/O on Windows.
 *
 * TIOCp inherits from TPollerBase and implements an asynchronous poller using the Windows
 * I/O Completion Ports (IOCP) API. It provides methods for posting asynchronous read, write,
 * accept, connect, and cancellation operations. To optimize performance, TIOCp uses a custom
 * arena allocator (@ref TArenaAllocator) to preallocate IOCP event structures, avoiding per-operation
 * dynamic memory allocations required by the API.
 *
 * Type aliases provided:
 * - @c TSocket is defined as NNet::TPollerDrivenSocket<TIOCp>.
 * - @c TFileHandle is defined as NNet::TPollerDrivenFileHandle<TIOCp>.
 *
 * Example usage:
 * @code{.cpp}
 * TIOCp iocpPoller;
 * // Register a socket:
 * iocpPoller.Register(socketFd);
 * // Post asynchronous operations, then call:
 * iocpPoller.Poll();
 * @endcode
 *
 * @note This class is specific to Windows and uses IOCP for high-performance I/O.
 */
class TIOCp: public TPollerBase {
public:
    /// Alias for the poller-driven socket type.
    using TSocket = NNet::TPollerDrivenSocket<TIOCp>;
    /// Alias for the poller-driven file handle type.
    using TFileHandle = NNet::TPollerDrivenFileHandle<TIOCp>;

    TIOCp();
    ~TIOCp();
    /**
     * @brief Posts an asynchronous read operation.
     *
     * @param fd   The file descriptor.
     * @param buf  Buffer to store read data.
     * @param size Number of bytes to read.
     * @param handle The coroutine handle to resume when the operation completes.
     */
    void Read(int fd, void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous write operation.
     *
     * @param fd   The file descriptor.
     * @param buf  Buffer containing data to write.
     * @param size Number of bytes to write.
     * @param handle The coroutine handle to resume when the operation completes.
     */
    void Write(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous receive operation.
     *
     * @param fd   The file descriptor.
     * @param buf  Buffer to store received data.
     * @param size Maximum number of bytes to receive.
     * @param handle The coroutine handle to resume when the operation completes.
     */
    void Recv(int fd, void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous send operation.
     *
     * @param fd   The file descriptor.
     * @param buf  Buffer containing data to send.
     * @param size Number of bytes to send.
     * @param handle The coroutine handle to resume when the operation completes.
     */
    void Send(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous accept operation.
     *
     * @param fd   The listening socket descriptor.
     * @param addr Pointer to a sockaddr structure to receive the client address.
     * @param len  Pointer to a variable specifying the size of the address structure.
     * @param handle The coroutine handle to resume when an incoming connection is accepted.
     */
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle);
    /**
     * @brief Posts an asynchronous connect operation.
     *
     * @param fd   The socket descriptor.
     * @param addr Pointer to the destination address.
     * @param len  Size of the destination address structure.
     * @param handle The coroutine handle to resume when the connection is established.
     */
    void Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle);
    /**
     * @brief Cancels all pending operations on the specified file descriptor.
     *
     * @param fd The file descriptor.
     */
    void Cancel(int fd);
    /**
     * @brief Cancels the pending operation posted with a coroutine handle.
     *
     * Called when the coroutine is destroyed while waiting for the operation.
     * Cancels it with CancelIoEx and returns only after it is complete (its
     * completion dequeued or its OVERLAPPED no longer pending), so its buffers
     * may be freed. The completion is dropped, so @p h is not resumed. Other
     * completions dequeued meanwhile are delivered by the next Poll().
     *
     * @param h The coroutine handle the operation was posted with.
     * @return The result of the dropped completion if it was collected already
     *         (e.g. an accepted socket), otherwise -1.
     */
    int Cancel(THandle h);
    /**
     * @brief Registers a file descriptor with the IOCP.
     *
     * @param fd The file descriptor to register.
     */
    void Register(int fd);
    /**
     * @brief Retrieves the result of the last completed IOCP operation.
     *
     * @return An integer representing the result of the operation.
     */
    int Result();
    /**
     * @brief Resumes the coroutines whose operations completed.
     *
     * Skips completions dropped by Cancel(THandle).
     */
    void WakeupReadyHandles();
    /**
     * @brief Polls for IOCP events.
     *
     * This method waits for asynchronous I/O events and processes their completion.
     */
    void Poll();

private:
    // OVERLAPPED is the first base: the system gets the operation as an OVERLAPPED*.
    struct TIO: OVERLAPPED, TIntrusiveNode<TIO> {
        THandle handle;
        struct sockaddr* addr = nullptr; // for accept
        socklen_t* len = nullptr; // for accept
        int sock = -1; // for accept
        int fd = -1;
        bool connect = false;

        TIO()
            : OVERLAPPED()
        { }
    };

    long GetTimeoutMs();
    TIO* NewTIO(int fd, THandle handle);
    void FreeTIO(TIO*);
    // Frees the operation of a dequeued completion, returns its handle and result.
    std::pair<THandle, int> Complete(const OVERLAPPED_ENTRY& entry);

    HANDLE Port_;

    // Allocator to avoid dynamic memory allocation for each IOCP event structure.
    TArenaAllocator<TIO> Allocator_;
    TIntrusiveList<TIO> Pending_; // operations whose completions are not dequeued yet
    std::vector<OVERLAPPED_ENTRY> Entries_;
    std::vector<int> Results_; // results of the completions in ReadyEvents_, by index
    size_t NextResult_ = 0;
    std::vector<std::pair<THandle, int>> Reaped_; // dequeued by Cancel(), delivered by the next Poll()
};

}
