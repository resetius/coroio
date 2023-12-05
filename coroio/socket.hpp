#pragma once

#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

#include "poller.hpp"

namespace NNet {

class TAddress {
public:
    TAddress(const std::string& addr, int port)
    {
        inet_pton(AF_INET, addr.c_str(), &(Addr_.sin_addr));
        Addr_.sin_port = htons(port);
        Addr_.sin_family = AF_INET;
    }

    TAddress(sockaddr_in addr)
        : Addr_(addr)
    { }

    TAddress() = default;

    auto Addr() const { return Addr_; }

    bool operator == (const TAddress& other) const {
        return memcmp(&Addr_, &other.Addr_, sizeof(Addr_)) == 0;
    }

private:
    struct sockaddr_in Addr_;
};

template<typename T>
struct TSockOpAwaitable {
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
        if (ret < 0 && !(errno==EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
            throw std::system_error(errno, std::generic_category());
        }
    }

    TPollerBase* poller;
    int fd;
    char* b; size_t s;
    int ret;
    bool ready;
};

template<typename TSockOps>
class TSocketBase {
public:
    TSocketBase(TPollerBase& poller)
        : Poller_(&poller)
        , Fd_(Create())
    { }

    TSocketBase(int fd, TPollerBase& poller)
        : Poller_(&poller)
        , Fd_(Setup(fd))
    { }

    TSocketBase() = default;

    auto ReadSome(char* buf, size_t size) {
        struct TAwaitableRead: public TSockOpAwaitable<TAwaitableRead> {
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
    auto ReadSomeYield(char* buf, size_t size) {
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

    auto WriteSome(char* buf, size_t size) {
        struct TAwaitableWrite: public TAwaitable<TAwaitableWrite> {
            void run() {
                this->ret = TSockOps::write(this->fd, this->b, this->s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                this->poller->AddWrite(this->fd, h);
            }
        };
        return TAwaitableWrite{Poller_,Fd_,buf,size};
    }

    auto WriteSomeYield(char* buf, size_t size) {
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
        return TAwaitableWrite{Poller_,Fd_,buf,size};
    }

protected:
    int Create() {
        auto s = socket(PF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            throw std::system_error(errno, std::generic_category(), "socket");
        }
        return Setup(s);
    }

    int Setup(int s) {
        struct stat statbuf;
        fstat(s, &statbuf);
        if (S_ISSOCK(statbuf.st_mode)) {
            int value;
            value = 1;
            if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(int)) < 0) {
                throw std::system_error(errno, std::generic_category(), "setsockopt");
            }
            value = 1;
            if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int)) < 0) {
                throw std::system_error(errno, std::generic_category(), "setsockopt");
            }
        }
        auto flags = fcntl(s, F_GETFL, 0);
        if (flags < 0) {
            throw std::system_error(errno, std::generic_category(), "fcntl");
        }
        if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::system_error(errno, std::generic_category(), "fcntl");
        }
        return s;
    }

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
            if (ret < 0 && !(errno==EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
                throw std::system_error(errno, std::generic_category());
            }
        }

        TPollerBase* poller;
        int fd;
        char* b; size_t s;
        int ret;
        bool ready;
    };

    TPollerBase* Poller_ = nullptr;
    int Fd_ = -1;
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
        return ::recv(fd, buf, count, 0);
    }

    static auto write(int fd, const void* buf, size_t count) {
        return ::send(fd, buf, count, 0);
    }
};

class TSocket: public TSocketBase<TSockOps> {
public:
    TSocket(TAddress&& addr, TPollerBase& poller)
        : TSocketBase(poller)
        , Addr_(std::move(addr))
    { }

    TSocket(const TAddress& addr, int fd, TPollerBase& poller)
        : TSocketBase(fd, poller)
        , Addr_(addr)
    { }

    TSocket(const TAddress& addr, TPollerBase& poller)
        : TSocketBase(poller)
        , Addr_(addr)
    { }

    TSocket(TSocket&& other)
    {
        *this = std::move(other);
    }

    ~TSocket()
    {
        if (Fd_ >= 0) {
            close(Fd_);
            Poller_->RemoveEvent(Fd_);
        }
    }

    TSocket() = default;
    TSocket(const TSocket& other) = delete;
    TSocket& operator=(TSocket& other) const = delete;

    TSocket& operator=(TSocket&& other) {
        if (this != &other) {
            Poller_ = other.Poller_;
            Addr_ = other.Addr_;
            Fd_ = other.Fd_;
            other.Fd_ = -1;
        }
        return *this;
    }

    auto Connect(TTime deadline = TTime::max()) {
        struct TAwaitable {
            bool await_ready() {
                int ret = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
                if (ret < 0 && !(errno == EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
                    throw std::system_error(errno, std::generic_category(), "connect");
                }
                return ret >= 0;
            }

            void await_suspend(std::coroutine_handle<> h) {
                poller->AddWrite(fd, h);
                if (deadline != TTime::max()) {
                    poller->AddTimer(fd, deadline, h);
                }
            }

            void await_resume() {
                if (deadline != TTime::max() && poller->RemoveTimer(fd, deadline)) {
                    throw std::system_error(std::make_error_code(std::errc::timed_out));
                }
            }

            TPollerBase* poller;
            int fd;
            sockaddr_in addr;
            TTime deadline;
        };
        return TAwaitable{Poller_, Fd_, Addr_.Addr(), deadline};
    }

    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->AddRead(fd, h);
            }
            TSocket await_resume() {
                sockaddr_in clientaddr;
                socklen_t len = sizeof(clientaddr);

                int clientfd = accept(fd, (sockaddr*)&clientaddr, &len);
                if (clientfd < 0) {
                    throw std::system_error(errno, std::generic_category(), "accept");
                }

                return TSocket{clientaddr, clientfd, *poller};
            }

            TPollerBase* poller;
            int fd;
        };

        return TAwaitable{Poller_, Fd_};
    }

    void Bind() {
        auto addr = Addr_.Addr();
        if (bind(Fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            throw std::system_error(errno, std::generic_category(), "bind");
        }
    }

    void Listen(int backlog = 128) {
        if (listen(Fd_, backlog) < 0) {
            throw std::system_error(errno, std::generic_category(), "listen");
        }
    }

    const TAddress& Addr() const {
        return Addr_;
    }

    int Fd() const {
        return Fd_;
    }

protected:
    TAddress Addr_;
};

} // namespace NNet
