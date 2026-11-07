#pragma once

#include <coroio/base.hpp>
#include <coroio/completions.hpp>
#include <coroio/socket.hpp>
#include <coroio/poller.hpp>

namespace NNet {

/**
 * @class TIOCp
 * @brief IOCP-based poller for asynchronous I/O on Windows.
 *
 * TIOCp inherits from TPollerBase and implements an asynchronous poller using the Windows
 * I/O Completion Ports (IOCP) API. It provides methods for posting asynchronous read, write,
 * accept, connect, and cancellation operations. The OVERLAPPED structure the API requires for
 * each operation is the operation's state (@ref TOperation), kept by the caller, so posting an
 * operation allocates no memory.
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
 * // Post an asynchronous receive. op must stay in place until it is delivered or cancelled.
 * TIOCp::TOperation op;
 * op.Handle = coroutineHandle;
 * iocpPoller.Recv(socketFd, buffer, bufferSize, op);
 * // Poll, then resume the coroutines of the completed operations. The result is in op.Result.
 * iocpPoller.Poll();
 * iocpPoller.WakeupReadyHandles();
 * @endcode
 *
 * @note This class is specific to Windows and uses IOCP for high-performance I/O.
 *
 * File handles (TFileHandle, Read(), Write()) take a CRT descriptor, as on the other
 * backends, e.g. from _open() or _open_osfhandle(). Closing the descriptor closes its handle.
 * - A handle opened with FILE_FLAG_OVERLAPPED is associated with the port at its first
 *   operation. A handle can be associated with one port only.
 * - Other handles, e.g. from _open(), the console or an inherited pipe, are read and written
 *   synchronously, as regular files on the readiness pollers: the loop waits for them, for
 *   a console read until Enter.
 * - On an overlapped disk file an operation takes [pointer, pointer + size) of the file
 *   pointer when posted, as if the operations posted before it transfer all their bytes:
 *   a write posted while a read at the end of the file is pending leaves a hole. An
 *   operation that transfers less gives the rest back unless a later one took the pointer,
 *   and a read leaves it at most at the end of the file, also after a seek past the end.
 * - Overlapped writes go to the end of the file only if the handle has FILE_APPEND_DATA and
 *   no FILE_WRITE_DATA access: _O_APPEND of _open_osfhandle() is a CRT flag they do not see.
 * - A read at the end of the file or of a pipe returns 0. A failed operation returns the
 *   negated Win32 error, or errno for a synchronous handle.
 * - The descriptor -1, e.g. of a closed TFileHandle, gives EBADF. Another invalid descriptor
 *   is checked by the CRT, as by read() on the other pollers: the UCRT ends the process.
 */
class TIOCp: public TPollerBase {
public:
    /// Alias for the poller-driven socket type.
    using TSocket = NNet::TPollerDrivenSocket<TIOCp>;
    /// Alias for the poller-driven file handle type. It takes a CRT descriptor, see TIOCp.
    using TFileHandle = NNet::TPollerDrivenFileHandle<TIOCp>;

    /**
     * @brief State of one operation, kept by the caller.
     *
     * Set TOperation::Handle to the coroutine to resume before posting the
     * operation; if posting throws, it is reset. The state is the operation's
     * OVERLAPPED, so it must stay in place until WakeupReadyHandles() delivers
     * the completion or Cancel() returns. The result of the completion is in
     * TOperation::Result.
     */
    struct TOperation: OVERLAPPED, NDetail::TOperationState {
        TOperation()
            : OVERLAPPED()
        { }

    private:
        friend class TIOCp;

        struct sockaddr* Addr = nullptr; // for accept
        socklen_t* Len = nullptr; // for accept
        int Sock = -1; // for accept
        int Fd = -1;
        bool Connect = false;
        HANDLE File = nullptr; // of a file associated with the port
        DWORD Size = 0; // bytes of a disk file's pointer taken when posted
        bool Read = false; // a file read: the end of the file or of a pipe gives 0 bytes

        // Resets the OVERLAPPED and every field except Handle for a new operation on fd.
        // A field added above must be reset here too.
        void Reset(int fd) {
            static_cast<OVERLAPPED&>(*this) = OVERLAPPED();
            Result = 0;
            Index = 0;
            Addr = nullptr;
            Len = nullptr;
            Sock = -1;
            Fd = fd;
            Connect = false;
            File = nullptr;
            Size = 0;
            Read = false;
        }
    };

    TIOCp();
    ~TIOCp();
    /**
     * @brief Posts an asynchronous read operation.
     *
     * @param fd   The CRT descriptor of the file, see TIOCp.
     * @param buf  Buffer to store read data.
     * @param size Number of bytes to read.
     * @param op   State of the operation, with the coroutine to resume when it completes.
     */
    void Read(int fd, void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous write operation.
     *
     * @param fd   The CRT descriptor of the file, see TIOCp.
     * @param buf  Buffer containing data to write.
     * @param size Number of bytes to write.
     * @param op   State of the operation, with the coroutine to resume when it completes.
     */
    void Write(int fd, const void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous receive operation.
     *
     * @param fd   The file descriptor.
     * @param buf  Buffer to store received data.
     * @param size Maximum number of bytes to receive.
     * @param op   State of the operation, with the coroutine to resume when it completes.
     */
    void Recv(int fd, void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous send operation.
     *
     * @param fd   The file descriptor.
     * @param buf  Buffer containing data to send.
     * @param size Number of bytes to send.
     * @param op   State of the operation, with the coroutine to resume when it completes.
     */
    void Send(int fd, const void* buf, int size, TOperation& op);
    /**
     * @brief Posts an asynchronous accept operation.
     *
     * @param fd   The listening socket descriptor.
     * @param addr Pointer to a sockaddr structure to receive the client address.
     * @param len  Pointer to a variable specifying the size of the address structure.
     * @param op   State of the operation, with the coroutine to resume when an incoming connection is accepted.
     */
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, TOperation& op);
    /**
     * @brief Posts an asynchronous connect operation.
     *
     * @param fd   The socket descriptor.
     * @param addr Pointer to the destination address.
     * @param len  Size of the destination address structure.
     * @param op   State of the operation, with the coroutine to resume when the connection is established.
     */
    void Connect(int fd, const sockaddr* addr, socklen_t len, TOperation& op);
    /**
     * @brief Cancels all pending operations on the specified socket.
     *
     * @param fd The socket.
     */
    void Cancel(int fd);
    /**
     * @brief Cancels a pending operation.
     *
     * Called when the coroutine is destroyed while waiting for the operation.
     * Cancels it with CancelIoEx and returns only after its completion is
     * dequeued, so its buffers and @p op may be freed. The completion is
     * dropped, so its coroutine is not resumed. Other completions dequeued
     * meanwhile are delivered by the next Poll().
     *
     * @param op State of the operation, not delivered yet.
     * @return The result of the dropped completion if it was collected already
     *         (e.g. an accepted socket), otherwise -1.
     */
    int Cancel(TOperation& op);
    /**
     * @brief Registers a socket with the IOCP.
     *
     * @param fd The socket to register.
     */
    void Register(int fd);
    /**
     * @brief Resumes the coroutines whose operations completed.
     *
     * Skips completions dropped by Cancel(TOperation&). A completion collected
     * by Poll() and not delivered here is kept by the next Poll().
     */
    void WakeupReadyHandles();
    /**
     * @brief Polls for IOCP events.
     *
     * This method waits for asynchronous I/O events and processes their completion.
     */
    void Poll();

private:
    long GetTimeoutMs();
    // Resets op for a new operation on fd, keeping its handle.
    static void Prepare(TOperation& op, int fd);
    // Resets op.Handle, since op is not posted, and throws.
    [[noreturn]] static void Throw(TOperation& op, int err, const std::error_category& category, const char* what);
    // Returns the result of the operation of a dequeued completion.
    int Complete(const OVERLAPPED_ENTRY& entry);
    // Returns the handle of CRT descriptor fd if it is associated with the port, nullptr if it
    // is synchronous. For a disk file takes size bytes of the file pointer for op.
    HANDLE StartFile(int fd, int size, TOperation& op);
    // Counts a file operation posted with result ok, or completes or throws if it failed at once.
    void PostedFile(TOperation& op, BOOL ok, const char* what);
    // Gives back the part of the file pointer that op took and did not transfer.
    void Rewind(TOperation& op, DWORD transferred);

    enum class EFile : unsigned char { Unknown, Port, Disk, Sync };
    std::vector<EFile> Files_; // by CRT descriptor, until the poller closes it

    HANDLE Port_;

    int InFlight_ = 0; // operations whose completions are not dequeued yet
    std::vector<OVERLAPPED_ENTRY> Entries_;
    NDetail::TCompletions Completions_;
};

}
