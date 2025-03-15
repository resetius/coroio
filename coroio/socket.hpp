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

#include <optional>
#include <variant>

#include "poller.hpp"
#include "address.hpp"

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
 *  - <tt>static int TSockOps::close(int fd)</tt>
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
     * When awaited, this method suspends the current coroutine until the read operation
     * completes and then returns the number of bytes read.
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
                this->poller->AddRead(this->fd, h);
            }
        };
        return TAwaitableRead{Poller_,Fd_,buf,size};
    }
    /**
     * @brief Forces a read operation on the next event loop iteration.
     *
     * Similar to @ref ReadSome(), but this variant ensures that the read is deferred
     * to the next iteration of the event loop.
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
                this->poller->AddRead(this->fd, h);
            }
        };
        return TAwaitableRead{Poller_,Fd_,buf,size};
    }
    /**
     * @brief Asynchronously writes data from the provided buffer to the socket.
     *
     * This method suspends the current coroutine until the data is written, and then returns
     * the number of bytes successfully written.
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
                this->poller->AddWrite(this->fd, h);
            }
        };
        return TAwaitableWrite{Poller_,Fd_,const_cast<void*>(buf),size};
    }
    /**
     * @brief Monitors the socket for remote hang-up (closure).
     *
     * This awaitable suspends the coroutine until a remote hang-up is detected.
     *
     * @return An awaitable object that yields a boolean value (true) once the remote hang-up
     *         is detected.
     */
    auto Monitor() {
        struct TAwaitableClose: public TAwaitable<TAwaitableClose> {
            void run() {
                this->ret = true;
            }

            void await_suspend(std::coroutine_handle<> h) {
                this->poller->AddRemoteHup(this->fd, h);
            }
        };
        return TAwaitableClose{Poller_,Fd_};
    }
    /**
     * @brief Closes the socket.
     *
     * Closes the socket using the low-level operation provided by TSockOps, removes
     * it from the poller, and marks the descriptor as invalid.
     */
    void Close()
    {
        if (Fd_ >= 0) {
            TSockOps::close(Fd_);
            Poller_->RemoveEvent(Fd_);
            Fd_ = -1;
        }
    }

protected:
    template<typename T>
    struct TAwaitable {
        bool await_ready() {
            SafeRun();
            return (ready = (ret >= 0));
        }

        int await_resume() {
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
    };
};

class TFileOps {
public:
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
 * The passed file descriptor is owned by TFileHandle and will be closed
 * automatically when the object is destroyed.
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
     */
    auto Connect(const TAddress& addr, TTime deadline = TTime::max()) {
        if (RemoteAddr_.has_value()) {
            throw std::runtime_error("Already connected");
        }
        RemoteAddr_ = addr;
        struct TAwaitable {
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
                return ret >= 0;
            }

            void await_suspend(std::coroutine_handle<> h) {
                poller->AddWrite(fd, h);
                if (deadline != TTime::max()) {
                    timerId = poller->AddTimer(deadline, h);
                }
            }

            void await_resume() {
                if (deadline != TTime::max() && poller->RemoveTimer(timerId, deadline)) {
                    throw std::system_error(std::make_error_code(std::errc::timed_out));
                }
            }

            TPollerBase* poller;
            int fd;
            std::pair<const sockaddr*, int> addr;
            TTime deadline;
            unsigned timerId = 0;
        };
        return TAwaitable{Poller_, Fd_, RemoteAddr_->RawAddr(), deadline};
    }
    /**
     * @brief Asynchronously accepts an incoming connection.
     *
     * This awaitable suspends until a new connection is ready on the socket and then returns
     * a new TSocket object representing the client connection.
     *
     * @return An awaitable object that yields a TSocket for the new connection.
     *
     * @throws std::system_error if accept() fails.
     */
    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->AddRead(fd, h);
            }
            TSocket await_resume() {
                char clientaddr[sizeof(sockaddr_in6)];
                socklen_t len = static_cast<socklen_t>(sizeof(sockaddr_in6));

                int clientfd = accept(fd, reinterpret_cast<sockaddr*>(&clientaddr[0]), &len);
                if (clientfd < 0) {
                    throw std::system_error(errno, std::generic_category(), "accept");
                }

                return TSocket{TAddress{reinterpret_cast<sockaddr*>(&clientaddr[0]), len}, clientfd, *poller};
            }

            TPollerBase* poller;
            int fd;
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

protected:
    std::optional<TAddress> LocalAddr_;
    std::optional<TAddress> RemoteAddr_;
};

/**
 * @class TPollerDrivenSocket
 * @brief Socket type driven by the poller's implementation.
 *
 * This class is used when socket operations (read, write, connect, etc.) are defined
 * by the poller—in other words, when these operations depend on the underlying poller
 * (e.g. for high-performance I/O using uring or IOCP).
 *
 * Ordinary users should not worry about selecting the appropriate socket type.
 * Instead, they should use the type defined by the poller, i.e. @c typename TPoller::TSocket.
 * Depending on the poller, this may resolve to either TSocket or TPollerDrivenSocket.
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
     * for the accepted connection.
     *
     * @return An awaitable yielding the accepted TPollerDrivenSocket.
     */
    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Accept(fd, reinterpret_cast<sockaddr*>(&addr[0]), &len, h);
            }

            TPollerDrivenSocket<T> await_resume() {
                int clientfd = poller->Result();
                if (clientfd < 0) {
                    throw std::system_error(-clientfd, std::generic_category(), "accept");
                }

                return TPollerDrivenSocket<T>{TAddress{reinterpret_cast<sockaddr*>(&addr[0]), len}, clientfd, *poller};
            }

            T* poller;
            int fd;

            char addr[2*(sizeof(sockaddr_in6)+16)] = {0}; // use additional memory for windows
            socklen_t len = static_cast<socklen_t>(sizeof(addr));
        };

        return TAwaitable{Poller_, Fd_};
    }

    /**
     * @brief Asynchronously connects to the specified address with an optional deadline.
     *
     * Initiates a connection and returns an awaitable that completes when the connection is established.
     * If a deadline is specified and exceeded, a timeout error is thrown.
     *
     * @param addr     The remote address to connect to.
     * @param deadline The timeout deadline (defaults to TTime::max()).
     * @return An awaitable object for the connection operation.
     *
     * @throws std::runtime_error if the socket is already connected.
     */
    auto Connect(const TAddress& addr, TTime deadline = TTime::max()) {
        if (RemoteAddr_.has_value()) {
            throw std::runtime_error("Already connected");
        }
        RemoteAddr_ = addr;
        struct TAwaitable {
            bool await_ready() const { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                poller->Connect(fd, addr.first, addr.second, h);
                if (deadline != TTime::max()) {
                    timerId = poller->AddTimer(deadline, h);
                }
            }

            void await_resume() {
                if (deadline != TTime::max() && poller->RemoveTimer(timerId, deadline)) {
                    poller->Cancel(fd);
                    throw std::system_error(std::make_error_code(std::errc::timed_out));
                }
                int ret = poller->Result();
                if (ret < 0) {
                    throw std::system_error(-ret, std::generic_category(), "connect");
                }
            }

            T* poller;
            int fd;
            std::pair<const sockaddr*, int> addr;
            TTime deadline;
            unsigned timerId = 0;
        };
        return TAwaitable{Poller_, Fd_, RemoteAddr()->RawAddr(), deadline};
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
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Recv(fd, buf, size, h);
            }

            auto await_resume() {
                auto ret = poller->Result();
                if (ret < 0) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            void* buf;
            size_t size;
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
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Send(fd, buf, size, h);
            }

            auto await_resume() {
                auto ret = poller->Result();
                if (ret < 0) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            const void* buf;
            size_t size;
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
     * The file descriptor is owned by the TPollerDrivenFileHandle and will be closed in the destructor.
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
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Read(fd, buf, size, h);
            }

            auto await_resume() {
                auto ret = poller->Result();
                if (ret < 0) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            void* buf;
            size_t size;
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
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Write(fd, buf, size, h);
            }

            auto await_resume() {
                auto ret = poller->Result();
                if (ret < 0) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            T* poller;
            int fd;

            const void* buf;
            size_t size;
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
