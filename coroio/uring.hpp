#pragma once

#include "base.hpp"
#include "socket.hpp"
#include "poller.hpp"

#include <liburing.h>
#include <assert.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/utsname.h>

#include <system_error>
#include <iostream>
#include <tuple>
#include <vector>
#include <coroutine>
#include <queue>

namespace NNet {

class TUringSocket;

class TUring: public TPollerBase {
public:
    using TSocket = NNet::TUringSocket;
    using TFileHandle = NNet::TFileHandle;

    TUring(int queueSize = 256);

    ~TUring();

    void Read(int fd, void* buf, int size, std::coroutine_handle<> handle);
    void Write(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle);
    void Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle);
    void Cancel(int fd);
    void Cancel(std::coroutine_handle<> h);

    int Wait(timespec ts = {10,0});

    void Poll() {
        Wait(GetTimeout());
    }

    int Result();

    void Submit();

    // for tests
    std::tuple<int, int, int> Kernel() const;
    const std::string& KernelStr() const;

private:
    io_uring_sqe* GetSqe() {
        io_uring_sqe* r = io_uring_get_sqe(&Ring_);
        if (!r) {
            Submit();
            r = io_uring_get_sqe(&Ring_);
        }
        return r;
    }

    int RingFd_;
    int EpollFd_;
    struct io_uring Ring_;
    std::queue<int> Results_;
    std::vector<char> Buffer_;
    std::tuple<int, int, int> Kernel_;
    std::string KernelStr_;
};

// TODO: XXX
class TUringSocket: public TSocket
{
public:
    using TPoller = TUring;

    TUringSocket(TAddress addr, TUring& poller)
        : TSocket(std::move(addr), poller)
        , Uring_(&poller)
    { }

    TUringSocket(const TAddress& addr, int fd, TUring& poller)
        : TSocket(addr, fd, poller)
        , Uring_(&poller)
    { }

    TUringSocket(int fd, TUring& poller)
        : TSocket({}, fd, poller)
        , Uring_(&poller)
    { }

    TUringSocket() = default;

    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Accept(fd, reinterpret_cast<sockaddr*>(&addr[0]), &len, h);
            }

            TUringSocket await_resume() {
                int clientfd = poller->Result();
                if (clientfd < 0) {
                    throw std::system_error(-clientfd, std::generic_category(), "accept");
                }

                return TUringSocket{TAddress{reinterpret_cast<sockaddr*>(&addr[0]), len}, clientfd, *poller};
            }

            TUring* poller;
            int fd;

            char addr[sizeof(sockaddr_in6)] = {0};
            socklen_t len = sizeof(sockaddr_in6);
        };

        return TAwaitable{Uring_, Fd_};
    }

    auto Connect(TTime deadline = TTime::max()) {
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

            TUring* poller;
            int fd;
            std::pair<const sockaddr*, int> addr;
            TTime deadline;
            unsigned timerId = 0;
        };
        return TAwaitable{Uring_, Fd_, Addr().RawAddr(), deadline};
    }

    auto ReadSome(void* buf, size_t size) {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Read(fd, buf, size, h);
            }

            ssize_t await_resume() {
                int ret = poller->Result();
                if (ret < 0) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            TUring* poller;
            int fd;

            void* buf;
            size_t size;
        };

        return TAwaitable{Uring_, Fd_, buf, size};
    }

    auto WriteSome(const void* buf, size_t size) {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Write(fd, buf, size, h);
            }

            ssize_t await_resume() {
                int ret = poller->Result();
                if (ret < 0) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            TUring* poller;
            int fd;

            const void* buf;
            size_t size;
        };

        return TAwaitable{Uring_, Fd_, buf, size};
    }

    auto WriteSomeYield(const void* buf, size_t size) {
        return WriteSome(buf, size);
    }

    auto ReadSomeYield(void* buf, size_t size) {
        return ReadSome(buf, size);
    }

private:
    TUring* Uring_;
};

} // namespace NNet
