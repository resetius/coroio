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

#include <variant>

#include "poller.hpp"

#ifdef _WIN32
int pipe(int pipes[2]);
int socketpair(int domain, int type, int protocol, SOCKET socks[2]);
int socketpair(int domain, int type, int protocol, int socks[2]);
#endif

namespace NNet {

class TInitializer {
public:
    TInitializer();
#ifdef _WIN32
    ~TInitializer();
#endif
};

class TAddress {
public:
    TAddress(const std::string& addr, int port);
    TAddress(sockaddr_in addr);
    TAddress(sockaddr_in6 addr);
    TAddress(sockaddr* addr, socklen_t len);
    TAddress() = default;

    const std::variant<sockaddr_in, sockaddr_in6>& Addr() const;
    std::pair<const sockaddr*, int> RawAddr() const;
    bool operator == (const TAddress& other) const;
    int Domain() const;
    TAddress WithPort(int port) const;

    std::string ToString() const;

private:
    std::variant<sockaddr_in, sockaddr_in6> Addr_ = {};
};

class TSocketOps {
public:
    TSocketOps(TPollerBase& poller, int domain, int type);
    TSocketOps(int fd, TPollerBase& poller);
    TSocketOps() = default;

    TPollerBase* Poller() { return Poller_; }

protected:
    int Create(int domain, int type);
    int Setup(int s);

    TPollerBase* Poller_ = nullptr;
    int Fd_ = -1;
};

template<typename TSockOps>
class TSocketBase: public TSocketOps {
public:
    TSocketBase(TPollerBase& poller, int domain, int type): TSocketOps(poller, domain, type)
    { }

    TSocketBase(int fd, TPollerBase& poller): TSocketOps(fd, poller)
    { }

    TSocketBase() = default;

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

    // force read on next loop iteration
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
};

class TFileHandle: public TSocketBase<TFileOps> {
public:
    TFileHandle(int fd, TPollerBase& poller)
        : TSocketBase(fd, poller)
    { }

    TFileHandle() = default;
};

class TSockOps {
public:
    static auto read(int fd, void* buf, size_t count) {
#ifdef _WIN32
        return ::recv(fd, static_cast<char*>(buf), count, 0);
#else
        return ::recv(fd, buf, count, 0);
#endif
    }

    static auto write(int fd, const void* buf, size_t count) {
#ifdef _WIN32
        return ::send(fd, static_cast<const char*>(buf), count, 0);
#else
        return ::send(fd, buf, count, 0);
#endif
    }
};

class TSocket: public TSocketBase<TSockOps> {
public:
    using TPoller = TPollerBase;

    TSocket(TAddress&& addr, TPollerBase& poller, int type = SOCK_STREAM);
    TSocket(const TAddress& addr, int fd, TPollerBase& poller);
    TSocket(const TAddress& addr, TPollerBase& poller, int type = SOCK_STREAM);
    TSocket(TSocket&& other);
    ~TSocket();

    TSocket() = default;
    TSocket(const TSocket& other) = delete;
    TSocket& operator=(TSocket& other) const = delete;

    TSocket& operator=(TSocket&& other);

    void Close();

    auto Connect(TTime deadline = TTime::max()) {
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
        return TAwaitable{Poller_, Fd_, Addr_.RawAddr(), deadline};
    }

    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->AddRead(fd, h);
            }
            TSocket await_resume() {
                char clientaddr[sizeof(sockaddr_in6)];
                socklen_t len = sizeof(sockaddr_in6);

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

    void Bind();
    void Listen(int backlog = 128);
    const TAddress& Addr() const;
    int Fd() const;

protected:
    TAddress Addr_;
};

} // namespace NNet
