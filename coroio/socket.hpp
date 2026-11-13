#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <expected>
#include <optional>
#include <variant>

#include "poller.hpp"
#include "address.hpp"
#include "corochain.hpp"

namespace NNet {

template<typename T> class TSocketBase;

/**
 * @class TSocketBase<void>
 * @brief Internal base class encapsulating a socket (or file) descriptor and its poller.
 *
 * This class stores a file descriptor and a pointer to a poller (TPollerBase) to support
 * asynchronous I/O operations. It provides functionality to create a new socket descriptor
 * (via @ref Create()) and to set it up (via @ref Setup()). Additionally, it holds the descriptor
 * and allows cleanup via @ref Close().
 *
 * @note This class is intended solely for internal use within the library. End-users should
 *       not interact directly with this class; instead, use the higher-level wrapper classes:
 *       @ref TSocket, @ref TFileHandle, @ref TPollerDrivenSocket, or @ref TPollerDrivenFileHandle.
 */
template<>
class TSocketBase<void> {
public:
    /**
     * @brief Returns the poller associated with this socket.
     * @return A pointer to the TPollerBase responsible for managing asynchronous events.
     */
    TPollerBase* Poller() { return Poller_; }

protected:
    TSocketBase(TPollerBase& poller, int domain, int type);
    TSocketBase(int fd, TPollerBase& poller);
    TSocketBase() = default;

    /**
     * @brief Creates a new socket descriptor.
     *
     * @param domain The protocol family of the socket.
     * @param type The type of socket.
     * @return The socket descriptor on success, or -1 on failure.
     */
    int Create(int domain, int type);
    /**
     * @brief Performs additional setup on the socket descriptor.
     *
     * This method may set socket options and configure the descriptor as needed.
     *
     * @param s The socket descriptor to set up.
     * @return A status code indicating success or failure.
     */
    int Setup(int s);

    TPollerBase* Poller_ = nullptr;
    int Fd_ = -1;
};

/**
 * @class TSocketBase
 * @brief Template base class implementing asynchronous socket I/O operations.
 *
 * This class extends the internal @ref TSocketBase<void> by implementing asynchronous
 * operations based on a type-specific socket operations trait, TSockOps. The TSockOps type
 * must implement the following static methods:
 *  - <tt>static int TSockOps::read(int fd, void* buf, size_t size)</tt>
 *  - <tt>static int TSockOps::write(int fd, const void* buf, size_t size)</tt>
 *  - <tt>static constexpr int TSockOps::CloseType</tt>, passed to TPollerBase::Close()
 *
 * It provides awaitable methods for reading and writing:
 *  - @ref ReadSome() and @ref ReadSomeYield() perform asynchronous reads.
 *  - @ref WriteSome() and @ref WriteSomeYield() perform asynchronous writes.
 *  - @ref Monitor() provides an awaitable to detect remote hang-ups.
 *
 * Both variants such as @ref TSocket and @ref TFileHandle (or similarly named higher-level wrappers)
 * inherit from this class and expose the more user-friendly API. End users should use these derived classes,
 * rather than interacting with TSocketBase<TSockOps> directly.
 *
 * @tparam TSockOps The type that provides low-level socket operations.
 */
template<typename TSockOps>
class TSocketBase: public TSocketBase<void> {
protected:
    /**
     * @brief Constructs a TSocketBase with a new socket descriptor.
     *
     * @param poller Reference to the poller responsible for asynchronous operations.
     * @param domain The address family (e.g., AF_INET, AF_INET6).
     * @param type The socket type (e.g., SOCK_STREAM, SOCK_DGRAM).
     */
    TSocketBase(TPollerBase& poller, int domain, int type): TSocketBase<void>(poller, domain, type)
    { }
    /**
     * @brief Constructs a TSocketBase from an existing socket descriptor.
     *
     * @param fd An already created socket descriptor.
     * @param poller Reference to the poller managing asynchronous I/O.
     */
    TSocketBase(int fd, TPollerBase& poller): TSocketBase<void>(fd, poller)
    { }

    TSocketBase() = default;
    TSocketBase(const TSocketBase& other) = delete;
    TSocketBase& operator=(TSocketBase& other) const = delete;

    ~TSocketBase() {
        Close();
    }

public:
    /**
     * @brief Asynchronously reads data from the socket into the provided buffer.
     *
     * Semantics: returns bytes read (>0), 0 on closure, and a negative value
     * (retry hint) for transient errors (e.g. EINTR/EAGAIN/EINPROGRESS). Non-retryable
     * errors throw std::system_error.
     *
     * @param buf Pointer to the destination buffer.
     * @param size The number of bytes to read.
     * @return An awaitable object that yields the number of bytes read.
     */
    auto ReadSome(void* buf, size_t size) {
        struct TAwaitableRead: public TAwaitable<TAwaitableRead> {
            void run() {
                this->ret = TSockOps::read(this->fd, this->b, this->s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                this->handle = h;
                this->poller->AddRead(this->fd, h);
            }
        };
        return TAwaitableRead{Poller_,Fd_,buf,size};
    }
    /**
     * @brief Forces a read operation on the next event loop iteration.
     *
     * Semantics: returns bytes read (>0), 0 on closure, and a negative value
     * (retry hint) for transient errors (e.g. EINTR/EAGAIN/EINPROGRESS). Non-retryable
     * errors throw std::system_error.
     *
     * @param buf Pointer to the buffer where data will be stored.
     * @param size Number of bytes to read.
     * @return An awaitable object yielding the number of bytes read.
     */
    auto ReadSomeYield(void* buf, size_t size) {
        struct TAwaitableRead: public TAwaitable<TAwaitableRead> {
            bool await_ready() {
                return (this->ready = false);
            }

            void run() {
                this->ret = TSockOps::read(this->fd, this->b, this->s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                this->handle = h;
                this->poller->AddRead(this->fd, h);
            }
        };
        return TAwaitableRead{Poller_,Fd_,buf,size};
    }
    /**
     * @brief Asynchronously writes data from the provided buffer to the socket.
     *
     * Semantics: returns bytes written (>0), 0 if nothing written, and a negative value
     * (retry hint) for transient errors (e.g. EINTR/EAGAIN/EINPROGRESS). Non-retryable
     * errors throw std::system_error.
     *
     * @param buf Pointer to the data to be written.
     * @param size The number of bytes to write.
     * @return An awaitable object that yields the number of bytes written.
     */
    auto WriteSome(const void* buf, size_t size) {
        struct TAwaitableWrite: public TAwaitable<TAwaitableWrite> {
            void run() {
                this->ret = TSockOps::write(this->fd, this->b, this->s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                this->handle = h;
                this->poller->AddWrite(this->fd, h);
            }
        };
        return TAwaitableWrite{Poller_,Fd_,const_cast<void*>(buf),size};
    }
    /**
     * @brief Forces a write operation on the next event loop iteration.
     *
     * This variant behaves similarly to @ref WriteSome() but defers execution until
     * the next loop iteration.
     *
     * @param buf Pointer to the data to be written.
     * @param size Number of bytes to write.
     * @return An awaitable object that yields the number of bytes written.
     */
    auto WriteSomeYield(const void* buf, size_t size) {
        struct TAwaitableWrite: public TAwaitable<TAwaitableWrite> {
            bool await_ready() {
                return (this->ready = false);
            }

            void run() {
                this->ret = TSockOps::write(this->fd, this->b, this->s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                this->handle = h;
                this->poller->AddWrite(this->fd, h);
            }
        };
        return TAwaitableWrite{Poller_,Fd_,const_cast<void*>(buf),size};
    }
    /**
     * @brief Monitors the socket for remote hang-up (closure).
     *
     * This awaitable suspends the coroutine until the peer closes the connection
     * or shuts down its writing side, or the descriptor reports an error, and
     * completes on the next loop iteration if that has already happened. Unread
     * data does not complete it, and reads and writes on the same socket may wait
     * at the same time.
     *
     * Only TEPoll, TKqueue and TPoll on Linux report remote hang-ups, see
     * TPollerBase::SupportsRemoteHup().
     *
     * @return An awaitable object that yields a boolean value (true) once the remote hang-up
     *         is detected.
     * @throws std::system_error with std::errc::operation_not_supported, at the call,
     *         on other pollers (TSelect, TPoll outside Linux, TUring, TIOCp).
     */
    auto Monitor() {
        if (!Poller_->SupportsRemoteHup()) {
            throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                "remote hang-up is not reported by this poller");
        }
        struct TAwaitableClose: public TAwaitable<TAwaitableClose> {
            bool await_ready() {
                return (this->ready = false);
            }

            void run() {
                this->ret = true;
            }

            void await_suspend(std::coroutine_handle<> h) {
                this->handle = h;
                this->poller->AddRemoteHup(this->fd, h);
            }
        };
        return TAwaitableClose{Poller_,Fd_};
    }
    /**
     * @brief Closes the socket.
     *
     * Hands the descriptor to TPollerBase::Close(), which closes it in the next
     * poll iteration: `co_await poller.Yield()` before binding a closed listener's
     * address again. Marks the descriptor as invalid.
     */
    void Close()
    {
        if (Fd_ >= 0) {
            Poller_->Close(Fd_, TSockOps::CloseType);
            Fd_ = -1;
        }
    }

protected:
    template<typename T>
    struct TAwaitable {
        ~TAwaitable() {
            if (handle) {
                // The coroutine is destroyed while waiting.
                poller->RemoveEvent(fd, handle);
            }
        }

        bool await_ready() {
            SafeRun();
            return (ready = (ret >= 0));
        }

        int await_resume() {
            handle = {};
            if (!ready) {
                SafeRun();
            }
            return ret;
        }

        void SafeRun() {
            ((T*)this)->run();
#ifdef _WIN32
            if (ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK ) {
                throw std::system_error(WSAGetLastError(), std::generic_category());
            }
#else
            if (ret < 0 && !(errno==EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
                throw std::system_error(errno, std::generic_category());
            }
#endif
        }

        TPollerBase* poller = nullptr;
        int fd = -1;
        void* b = nullptr; size_t s = 0;
        int ret = -1;
        bool ready = false;
        THandle handle = {};
    };
};

class TFileOps {
public:
    static constexpr int CloseType = TEvent::CLOSE_FILE; ///< Passed to TPollerBase::Close().

    static auto read(int fd, void* buf, size_t count) {
        return ::read(fd, buf, count);
    }

    static auto write(int fd, const void* buf, size_t count) {
        return ::write(fd, buf, count);
    }

    static auto close(int fd) {
        return ::close(fd);
    }
};

/**
 * @class TFileHandle
 * @brief Asynchronous file handle that owns its file descriptor.
 *
 * The passed file descriptor is owned by TFileHandle: the destructor hands it to
 * the poller, which closes it in its next iteration.
 */
class TFileHandle: public TSocketBase<TFileOps> {
public:
    /**
     * @brief Constructs a TFileHandle from an existing file descriptor.
     *
     * @param fd    The file descriptor to be managed.
     * @param poller Reference to a poller for asynchronous operations.
     */
    TFileHandle(int fd, TPollerBase& poller)
        : TSocketBase(fd, poller)
    { }

    TFileHandle(TFileHandle&& other);
    TFileHandle& operator=(TFileHandle&& other);

    TFileHandle() = default;
};

class TSockOps {
public:
    static constexpr int CloseType = TEvent::CLOSE_SOCKET; ///< Passed to TPollerBase::Close().

    static auto read(int fd, void* buf, size_t count) {
        return ::recv(fd, static_cast<char*>(buf), count, 0);
    }

    static auto write(int fd, const void* buf, size_t count) {
        return ::send(fd, static_cast<const char*>(buf), count, 0);
    }

    static auto close(int fd) {
        if (fd >= 0) {
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
        }
    }
};

/**
 * @class TSocket
 * @brief High-level asynchronous socket for network communication.
 *
 * TSocket provides an easy-to-use interface for common
 * network operations such as connecting, accepting, binding, and listening. It also stores
 * the local and remote addresses.
 */
class TSocket: public TSocketBase<TSockOps> {
public:
    using TPoller = TPollerBase;

    TSocket() = default;

    /**
     * @brief Constructs a TSocket using the given poller, address family, and socket type.
     *
     * @param poller Reference to the poller for asynchronous operations.
     * @param domain The address family (e.g., AF_INET, AF_INET6).
     * @param type   The socket type; defaults to SOCK_STREAM.
     */
    TSocket(TPollerBase& poller, int domain, int type = SOCK_STREAM);
    /**
     * @brief Constructs a TSocket for an already-connected socket.
     *
     * @param addr The remote address.
     * @param fd   The existing socket descriptor.
     * @param poller Reference to the poller.
     */
    TSocket(const TAddress& addr, int fd, TPollerBase& poller);

    TSocket(TSocket&& other);
    TSocket& operator=(TSocket&& other);

    /**
     * @brief Asynchronously connects to the specified address.
     *
     * Initiates a connection to @p addr. If the socket is already connected, an error is thrown.
     * A deadline may be specified to time out the connection.
     *
     * @param addr     The destination address.
     * @param deadline (Optional) Timeout deadline; defaults to TTime::max().
     * @return An awaitable object that suspends until the connection is established.
     *
     * @note If a deadline is specified and exceeded, the awaitable will throw a timed-out error.
     * @throws std::runtime_error at the call, before anything is awaited, if the socket is already connected.
     * @throws std::system_error at @c co_await with the socket error if the connection attempt fails.
     */
    auto Connect(const TAddress& addr, TTime deadline = TTime::max()) {
        if (RemoteAddr_.has_value()) {
            throw std::runtime_error("Already connected");
        }
        RemoteAddr_ = addr;
        struct TAwaitable {
            ~TAwaitable() {
                if (handle) {
                    poller->RemoveEvent(fd, handle);
                    if (deadline != TTime::max()) {
                        poller->RemoveTimer(timerId, deadline);
                    }
                }
            }

            bool await_ready() {
                int ret = connect(fd, addr.first, addr.second);
#ifdef _WIN32
                if (ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
                    throw std::system_error(WSAGetLastError(), std::generic_category(), "connect");
                }
#else
                if (ret < 0 && !(errno == EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
                    throw std::system_error(errno, std::generic_category(), "connect");
                }
#endif
                return (ready = (ret >= 0));
            }

            void await_suspend(std::coroutine_handle<> h) {
                handle = h;
                poller->AddWrite(fd, h);
                if (deadline != TTime::max()) {
                    timerId = poller->AddTimer(deadline, h);
                }
            }

            void await_resume() {
                handle = {};
                if (ready) {
                    return;
                }
                if (deadline != TTime::max() && poller->RemoveTimer(timerId, deadline)) {
                    poller->RemoveEvent(fd);
                    throw std::system_error(std::make_error_code(std::errc::timed_out));
                }
                // Writability only means the attempt has finished, SO_ERROR tells how.
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) < 0) {
#ifdef _WIN32
                    err = WSAGetLastError();
#else
                    err = errno;
#endif
                }
                if (err != 0) {
                    throw std::system_error(err, std::generic_category(), "connect");
                }
            }

            TPollerBase* poller;
            int fd;
            std::pair<const sockaddr*, int> addr;
            TTime deadline;
            unsigned timerId = 0;
            bool ready = false;
            THandle handle = {};
        };
        return TAwaitable{Poller_, Fd_, RemoteAddr_->RawAddr(), deadline};
    }
    /**
     * @brief Asynchronously accepts an incoming connection.
     *
     * This awaitable suspends until a new connection is ready on the socket and then returns
     * a new TSocket object representing the client connection.
     *
     * Errors of accept() are returned, not thrown: a server must keep accepting after
     * them. After a transient one, see IsTransientAcceptError(), accept again at once.
     * After others, e.g. EMFILE, the socket may stay readable: pause first.
     *
     * @return An awaitable object that yields a TSocket for the new connection, or the
     *         error code of accept() (errno; WSAGetLastError() on Windows).
     *
     * @throws std::system_error with EBADF at @c co_await if the socket is closed.
     */
    auto Accept() {
        struct TAwaitable {
            ~TAwaitable() {
                if (handle) {
                    poller->RemoveEvent(fd, handle);
                }
            }
            bool await_ready() const {
                if (fd < 0) {
                    throw std::system_error(EBADF, std::generic_category(), "accept");
                }
                return false;
            }
            void await_suspend(std::coroutine_handle<> h) {
                handle = h;
                poller->AddRead(fd, h);
            }
            std::expected<TSocket, int> await_resume() {
                handle = {};
                char clientaddr[sizeof(sockaddr_in6)];
                socklen_t len = static_cast<socklen_t>(sizeof(sockaddr_in6));

#ifdef SOCK_CLOEXEC
                int clientfd = accept4(fd, reinterpret_cast<sockaddr*>(&clientaddr[0]), &len, SOCK_CLOEXEC);
#else
                int clientfd = accept(fd, reinterpret_cast<sockaddr*>(&clientaddr[0]), &len);
#endif
                if (clientfd < 0) {
#ifdef _WIN32
                    // A closed socket is caught in await_ready.
                    int err = WSAGetLastError();
#else
                    int err = errno;
                    if (err == EBADF) {
                        throw std::system_error(err, std::generic_category(), "accept");
                    }
#endif
                    return std::unexpected(err);
                }
#if !defined(SOCK_CLOEXEC) && !defined(_WIN32)
                if (fcntl(clientfd, F_SETFD, FD_CLOEXEC) < 0) {
                    auto err = errno;
                    ::close(clientfd);
                    throw std::system_error(err, std::generic_category(), "fcntl");
                }
#endif

                return TSocket{TAddress{reinterpret_cast<sockaddr*>(&clientaddr[0]), len}, clientfd, *poller};
            }

            TPollerBase* poller;
            int fd;
            THandle handle = {};
        };

        return TAwaitable{Poller_, Fd_};
    }

    /// Binds the socket to the specified local address.
    void Bind(const TAddress& addr);
    /// Puts the socket in a listening state with an optional backlog (default is 128).
    void Listen(int backlog = 128);
    /**
     * @brief Returns the remote address of the connected peer.
     *
     * @return An optional TAddress holding the remote peer's address.
     */
    const std::optional<TAddress>& RemoteAddr() const;
    /**
     * @brief Returns the local address to which the socket is bound.
     *
     * @return An optional TAddress holding the local address.
     */
    const std::optional<TAddress>& LocalAddr() const;
    /// Returns the underlying socket descriptor.
    int Fd() const;
    /**
     * @brief Disables Nagle's algorithm (TCP_NODELAY) on a TCP socket.
     *
     * Best effort: returns false, and leaves the socket usable, if the option cannot be set
     * (not a TCP socket, or a connection already reset by the peer).
     */
    bool SetNoDelay();

protected:
    std::optional<TAddress> LocalAddr_;
    std::optional<TAddress> RemoteAddr_;
};

/**
 * @class TPollerDrivenSocket
 * @brief Socket type driven by the poller's implementation.
 *
 * This class is used when socket operations (read, write, connect, etc.) are defined
 * by the poller, in other words, when these operations depend on the underlying poller
 * (e.g. for high-performance I/O using uring or IOCP).
 *
 * Ordinary users should not worry about selecting the appropriate socket type.
 * Instead, they should use the type defined by the poller, i.e. @c typename TPoller::TSocket.
 * Depending on the poller, this may resolve to either TSocket or TPollerDrivenSocket.
 *
 * @note The poller must outlive every coroutine suspended on an operation of this
 *       class: destroying such a coroutine cancels the operation in the poller.
 *
 * @note Unlike TSocket::Connect, TPollerDrivenSocket::Connect is a coroutine that starts running
 * at the call, so "Already connected" is not thrown at the call: it is stored in the returned
 * future and rethrown at @c co_await. A future that is never awaited drops the error. Always
 * await the result of Connect, and put the try/catch around the @c co_await.
 *
 * @tparam T The poller type used by this socket. The poller must provide methods for
 * asynchronous operations (such as Connect, Accept, Recv, and Send) and timer management.
 */
template<typename T>
class TPollerDrivenSocket: public TSocket
{
public:
    using TPoller = T;

    /**
     * @brief Constructs a TPollerDrivenSocket from a poller, domain, and socket type.
     *
     * The socket is registered with the poller upon construction.
     *
     * @param poller Reference to the poller handling asynchronous events.
     * @param domain The address family (e.g., AF_INET, AF_INET6).
     * @param type   The socket type (defaults to SOCK_STREAM).
     */
    TPollerDrivenSocket(T& poller, int domain, int type = SOCK_STREAM)
        : TSocket(poller, domain, type)
        , Poller_(&poller)
    {
        Poller_->Register(Fd_);
    }

    /**
     * @brief Constructs a TPollerDrivenSocket from an existing file descriptor.
     *
     * @param fd     The socket descriptor.
     * @param poller Reference to the poller.
     */
    TPollerDrivenSocket(const TAddress& addr, int fd, T& poller)
        : TSocket(addr, fd, poller)
        , Poller_(&poller)
    {
        Poller_->Register(Fd_);
    }

    TPollerDrivenSocket(int fd, T& poller)
        : TSocket({}, fd, poller)
        , Poller_(&poller)
    {
        Poller_->Register(Fd_);
    }

    TPollerDrivenSocket() = default;

    /**
     * @brief Asynchronously accepts an incoming connection.
     *
     * Returns an awaitable object that, upon completion, yields a new TPollerDrivenSocket
     * for the accepted connection. Errors are handled as in TSocket::Accept().
     *
     * An accept that is pending when the socket is closed returns the cancel error as a value
     * (995 on IOCP, ECANCELED on io_uring).
     *
     * @return An awaitable yielding the accepted TPollerDrivenSocket, or the error code.
     * @throws std::system_error with EBADF at @c co_await if the socket was already closed.
     */
    auto Accept() {
        struct TAwaitable {
            ~TAwaitable() {
                if (op.Handle) {
                    // The coroutine is destroyed while waiting. Close a connection accepted meanwhile.
                    TSockOps::close(poller->Cancel(op));
                }
            }

            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                op.Handle = h;
                poller->Accept(fd, reinterpret_cast<sockaddr*>(&addr[0]), &len, op);
            }

            std::expected<TPollerDrivenSocket<T>, int> await_resume() {
                int clientfd = op.Result;
                if (clientfd == -EBADF) {
                    throw std::system_error(EBADF, std::generic_category(), "accept");
                }
                if (clientfd < 0) {
                    return std::unexpected(-clientfd);
                }

                return TPollerDrivenSocket<T>{TAddress{reinterpret_cast<sockaddr*>(&addr[0]), len}, clientfd, *poller};
            }

            T* poller;
            int fd;

            char addr[2*(sizeof(sockaddr_in6)+16)] = {0}; // use additional memory for windows
            socklen_t len = static_cast<socklen_t>(sizeof(addr));
            typename T::TOperation op = {};
        };

        return TAwaitable{Poller_, Fd_};
    }

    /**
     * @brief Asynchronously connects to the specified address with an optional deadline.
     *
     * Starts the connection at once and returns a future that completes when the connection is established.
     * If a deadline is specified and exceeded, a timeout error is thrown.
     *
     * @param addr     The remote address to connect to.
     * @param deadline The timeout deadline (defaults to TTime::max()).
     * @return A future for the connection operation. It must be awaited: every error below is stored in it.
     *
     * @throws std::runtime_error at @c co_await, not at the call, if the socket is already connected.
     * @throws std::system_error at @c co_await with the socket error if the connection attempt fails,
     *         or with std::errc::timed_out if the deadline is exceeded.
     */
    TFuture<void> Connect(const TAddress& addr, TTime deadline = TTime::max()) {
        if (RemoteAddr_.has_value()) {
            throw std::runtime_error("Already connected");
        }
        RemoteAddr_ = addr;
        struct TAwaitable {
            ~TAwaitable() {
                if (op.Handle) {
                    poller->Cancel(op);
                    if (deadline != TTime::max()) {
                        poller->RemoveTimer(timerId, deadline);
                    }
                }
            }

            bool await_ready() const { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                op.Handle = h;
                poller->Connect(fd, addr.first, addr.second, op);
                if (deadline != TTime::max()) {
                    timerId = poller->AddTimer(deadline, h);
                }
            }

            int await_resume() {
                if (deadline != TTime::max() && poller->RemoveTimer(timerId, deadline)) {
                    // The deadline fired first: the connect is still pending, or its completion is not delivered yet.
                    poller->Cancel(op);
                    throw std::system_error(std::make_error_code(std::errc::timed_out));
                }
                return op.Result;
            }

            T* poller;
            int fd;
            std::pair<const sockaddr*, int> addr;
            TTime deadline;
            unsigned timerId = 0;
            typename T::TOperation op = {};
        };
        int ret = co_await TAwaitable{Poller_, Fd_, RemoteAddr()->RawAddr(), deadline};
        if (ret < 0) {
            throw std::system_error(-ret, std::generic_category(), "connect");
        }
    }

    /**
     * @brief Asynchronously reads data from the socket.
     *
     * Returns an awaitable that suspends until data is available, then returns the number
     * of bytes read.
     *
     * @param buf  Pointer to the buffer where data will be stored.
     * @param size Number of bytes to read.
     * @return An awaitable yielding the number of bytes read.
     */
    auto ReadSome(void* buf, size_t size) {
        struct TAwaitable {
            ~TAwaitable() {
                if (op.Handle) {
                    poller->Cancel(op);
                }
            }

            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                op.Handle = h;
                poller->Recv(fd, buf, size, op);
            }

            auto await_resume() {
                auto ret = op.Result;
                if (ret < 0) {
#ifdef _WIN32
                    int err = -ret;
                    if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS) {
                        return ret; // retry hint
                    }
#else
                    int err = -ret;
                    if (err == EINTR || err == EAGAIN || err == EINPROGRESS) {
                        return ret; // retry hint
                    }
#endif
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            void* buf;
            size_t size;
            typename T::TOperation op = {};
        };

        return TAwaitable{Poller_, Fd_, buf, size};
    }

    /**
     * @brief Asynchronously writes data to the socket.
     *
     * Returns an awaitable that suspends until data is written, then returns the number
     * of bytes written.
     *
     * @param buf  Pointer to the data to write.
     * @param size Number of bytes to write.
     * @return An awaitable yielding the number of bytes written.
     */
    auto WriteSome(const void* buf, size_t size) {
        struct TAwaitable {
            ~TAwaitable() {
                if (op.Handle) {
                    poller->Cancel(op);
                }
            }

            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                op.Handle = h;
                poller->Send(fd, buf, size, op);
            }

            auto await_resume() {
                auto ret = op.Result;
                if (ret < 0) {
#ifdef _WIN32
                    int err = -ret;
                    if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS) {
                        return ret; // retry hint
                    }
#else
                    int err = -ret;
                    if (err == EINTR || err == EAGAIN || err == EINPROGRESS) {
                        return ret; // retry hint
                    }
#endif
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            const void* buf;
            size_t size;
            typename T::TOperation op = {};
        };

        return TAwaitable{Poller_, Fd_, buf, size};
    }

    /// The WriteSomeYield and ReadSomeYield variants behave similarly to WriteSome/ReadSome.
    auto WriteSomeYield(const void* buf, size_t size) {
        return WriteSome(buf, size);
    }

    /// The WriteSomeYield and ReadSomeYield variants behave similarly to WriteSome/ReadSome.
    auto ReadSomeYield(void* buf, size_t size) {
        return ReadSome(buf, size);
    }

private:
    T* Poller_;
};

/**
 * @class TPollerDrivenFileHandle
 * @brief Asynchronous file handle driven by the poller's implementation.
 *
 * This class implements file I/O operations (e.g. read, write)
 * using methods provided by the poller. In such scenarios, the poller controls the behavior
 * of these operations (for example, when using io_uring on Linux or IOCP on Windows).
 *
 * Ordinary users should not choose a file handle type directly. Instead, they should use the type
 * defined by the poller (i.e. @c typename TPoller::TFileHandle), which will resolve to either
 * TFileHandle or TPollerDrivenFileHandle depending on the poller in use.
 *
 * @note The poller must outlive every coroutine suspended on an operation of this
 *       class: destroying such a coroutine cancels the operation in the poller.
 *
 * @note On TIOCp the descriptor is a CRT descriptor, and some handles are read and written
 *       synchronously: see TIOCp.
 *
 * @tparam T The poller type that defines the asynchronous file operations.
 */
template<typename T>
class TPollerDrivenFileHandle: public TFileHandle
{
public:
    using TPoller = T;

    /**
     * @brief Constructs a TPollerDrivenFileHandle from an existing file descriptor.
     *
     * The file descriptor is owned by the TPollerDrivenFileHandle: the destructor hands it to the
     * poller, which closes it in its next iteration.
     *
     * @param fd     The file descriptor to manage.
     * @param poller Reference to the poller for asynchronous I/O.
     */
    TPollerDrivenFileHandle(int fd, T& poller)
        : TFileHandle(fd, poller)
        , Poller_(&poller)
    { }

    /**
     * @brief Asynchronously reads data from the file into the provided buffer.
     *
     * Returns an awaitable object that suspends until data is available, then returns the number
     * of bytes read.
     *
     * @param buf  Pointer to the buffer where the data will be stored.
     * @param size The number of bytes to read.
     * @return An awaitable yielding the number of bytes read.
     */
    auto ReadSome(void* buf, size_t size) {
        struct TAwaitable {
            ~TAwaitable() {
                if (op.Handle) {
                    poller->Cancel(op);
                }
            }

            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                op.Handle = h;
                poller->Read(fd, buf, size, op);
            }

            auto await_resume() {
                auto ret = op.Result;
                if (ret < 0) {
#ifdef _WIN32
                    int err = -ret;
                    if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS) {
                        return ret; // retry hint
                    }
#else
                    int err = -ret;
                    if (err == EINTR || err == EAGAIN || err == EINPROGRESS) {
                        return ret; // retry hint
                    }
#endif
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            void* buf;
            size_t size;
            typename T::TOperation op = {};
        };

        return TAwaitable{Poller_, Fd_, buf, size};
    }

    /**
     * @brief Asynchronously writes data from the provided buffer to the file.
     *
     * Returns an awaitable object that suspends until the data is written, then returns the number
     * of bytes written.
     *
     * @param buf  Pointer to the data to write.
     * @param size The number of bytes to write.
     * @return An awaitable yielding the number of bytes written.
     */
    auto WriteSome(const void* buf, size_t size) {
        struct TAwaitable {
            ~TAwaitable() {
                if (op.Handle) {
                    poller->Cancel(op);
                }
            }

            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                op.Handle = h;
                poller->Write(fd, buf, size, op);
            }

            auto await_resume() {
                auto ret = op.Result;
                if (ret < 0) {
#ifdef _WIN32
                    int err = -ret;
                    if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS) {
                        return ret; // retry hint
                    }
#else
                    int err = -ret;
                    if (err == EINTR || err == EAGAIN || err == EINPROGRESS) {
                        return ret; // retry hint
                    }
#endif
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            const void* buf;
            size_t size;
            typename T::TOperation op = {};
        };

        return TAwaitable{Poller_, Fd_, buf, size};
    }

    /// The WriteSomeYield and ReadSomeYield variants behave similarly to WriteSome/ReadSome.
    auto WriteSomeYield(const void* buf, size_t size) {
        return WriteSome(buf, size);
    }

    /// The WriteSomeYield and ReadSomeYield variants behave similarly to WriteSome/ReadSome.
    auto ReadSomeYield(void* buf, size_t size) {
        return ReadSome(buf, size);
    }

private:
    T* Poller_;
};

} // namespace NNet
