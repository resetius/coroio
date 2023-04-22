#pragma once

#include "socket.hpp"

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <system_error>
#include <iostream>
#include <vector>
#include <coroutine>
#include <queue>

namespace NNet {

class TUring {
public:
    TUring(int queueSize)
        : RingFd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
        , EpollFd_(epoll_create1(EPOLL_CLOEXEC))
    {
        int err;
        if (RingFd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "eventfd");
        }
        if (EpollFd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
        if ((err = io_uring_queue_init(queueSize, &Ring_, 0)) < 0) {
            throw std::system_error(-err, std::generic_category(), "io_uring_queue_init");
        }
        if ((err = io_uring_register_eventfd(&Ring_, RingFd_)) < 0) {
            throw std::system_error(-err, std::generic_category(), "io_uring_register_eventfd");
        }

        epoll_event eev = {};
        eev.data.fd = RingFd_;
        eev.events  = EPOLLIN;
        if (epoll_ctl(EpollFd_, EPOLL_CTL_ADD, eev.data.fd, &eev) < 0) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
    }

    ~TUring() {
        io_uring_queue_exit(&Ring_);
        close(RingFd_);
        close(EpollFd_);
    }

    void Read(int fd, char* buf, int size, std::coroutine_handle<> handle) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&Ring_); // TODO: check result
        io_uring_prep_read(sqe, fd, buf, size, 0);
        Submit(sqe, handle);
    }

    void Write(int fd, char* buf, int size, std::coroutine_handle<> handle) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&Ring_); // TODO: check result
        io_uring_prep_write(sqe, fd, buf, size, 0);
        Submit(sqe, handle);
    }

    void Accept(int fd, struct sockaddr_in* addr, socklen_t* len, std::coroutine_handle<> handle) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&Ring_); // TODO: check result
        io_uring_prep_accept(sqe, fd, reinterpret_cast<struct sockaddr*>(addr), len, 0);
        Submit(sqe, handle);
    }

    int Wait() {
        struct io_uring_cqe *cqe;
        unsigned head;
        int err;

        int nfds = 0;
        int timeout = 1000; // ms
        epoll_event outEvents[1];

        while ((nfds =  epoll_wait(EpollFd_, &outEvents[0], 1, timeout)) < 0) {
            if (errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "epoll_wait");
            }
        }

        if (nfds == 1) {
            eventfd_t v;
            eventfd_read(RingFd_, &v);
        }

        struct __kernel_timespec ts = {0, 0};
        if ((err = io_uring_wait_cqe_timeout(&Ring_, &cqe, &ts)) < 0) {
            if (-err != ETIME) {
                throw std::system_error(-err, std::generic_category(), "io_uring_wait_cqe_timeout");
            }
        }

        int completed = 0;
        io_uring_for_each_cqe(&Ring_, head, cqe) {
            completed ++;
            void* data = reinterpret_cast<void*>(cqe->user_data);
            if (data != nullptr) {
                std::coroutine_handle<> handle = std::coroutine_handle<>::from_address(data);
                Results_.push(cqe->res);
                handle.resume();
            }
        }

        io_uring_cq_advance(&Ring_, completed);
        return completed;
    }

    int Result() {
        int r = Results_.front();
        Results_.pop();
        return r;
    }

private:
    void Submit(io_uring_sqe *sqe, std::coroutine_handle<> handle) {
        io_uring_sqe_set_data(sqe, handle.address());
        int err;
        if ((err = io_uring_submit(&Ring_) < 0)) {
            throw std::system_error(-err, std::generic_category(), "io_uring_submit");
        }
    }

    int RingFd_;
    int EpollFd_;
    struct io_uring Ring_;
    std::queue<int> Results_;
};

// TODO: XXX
class TUringSocket: public TSocket
{
public:
    TUringSocket(TAddress&& addr, TUring& poller)
        : TSocket(addr, *(TPollerBase*)nullptr) // TODO: XXX
        , Uring_(&poller)
    { }

    TUringSocket(const TAddress& addr, int fd, TUring& poller) // TODO: XXX
        : TSocket(addr, fd, *(TPollerBase*)nullptr)
        , Uring_(&poller)
    { }

    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Accept(fd, &addr, &len, h);
            }

            TUringSocket await_resume() {
                int clientfd = poller->Result();
                if (clientfd < 0) {
                    throw std::system_error(-clientfd, std::generic_category(), "accept");
                }

                return TUringSocket{addr, clientfd, *poller};
            }

            TUring* poller;
            int fd;

            sockaddr_in addr;
            socklen_t len = sizeof(addr);
        };

        return TAwaitable{Uring_, Fd_};
    }

    auto ReadSome(char* buf, size_t size) {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Read(fd, buf, size, h);
            }

            ssize_t await_resume() {
                int ret = poller->Result();
                if (ret < 0 && !(-ret==EINTR||-ret==EAGAIN||-ret==EINPROGRESS)) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            TUring* poller;
            int fd;

            char* buf;
            size_t size;
        };

        return TAwaitable{Uring_, Fd_, buf, size};
    }

    auto WriteSome(char* buf, size_t size) {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->Write(fd, buf, size, h);
            }

            ssize_t await_resume() {
                int ret = poller->Result();
                if (ret < 0 && !(-ret==EINTR||-ret==EAGAIN||-ret==EINPROGRESS)) {
                    throw std::system_error(-ret, std::generic_category());
                }
                return ret;
            }

            TUring* poller;
            int fd;

            char* buf;
            size_t size;
        };

        return TAwaitable{Uring_, Fd_, buf, size};
    }

private:
    TUring* Uring_;
};

} // namespace NNet
