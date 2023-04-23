#pragma once

#include "socket.hpp"
#include "poller.hpp"

#include <liburing.h>
#include <assert.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <system_error>
#include <iostream>
#include <vector>
#include <coroutine>
#include <queue>

namespace NNet {

class TUringSocket;

class TUring: public TPollerBase {
public:
    using TSocket = NNet::TUringSocket;

    TUring(int queueSize = 256)
        : RingFd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
        , EpollFd_(epoll_create1(EPOLL_CLOEXEC))
        , Buffer_(32768)
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
//        if ((err = io_uring_register_eventfd(&Ring_, RingFd_)) < 0) {
//            throw std::system_error(-err, std::generic_category(), "io_uring_register_eventfd");
//        }

//        epoll_event eev = {};
//        eev.data.fd = RingFd_;
//        eev.events  = EPOLLIN;
//        if (epoll_ctl(EpollFd_, EPOLL_CTL_ADD, eev.data.fd, &eev) < 0) {
//            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
//        }

//        iovec iov = {.iov_base = Buffer_.data(), .iov_len = Buffer_.size() };
//        if ((err = io_uring_register_buffers(&Ring_, &iov, 1)) < 0) {
//            throw std::system_error(-err, std::generic_category(), "io_uring_register_buffers");
//        }
    }

    ~TUring() {
        io_uring_queue_exit(&Ring_);
        close(RingFd_);
        close(EpollFd_);
    }

    void Read(int fd, char* buf, int size, std::coroutine_handle<> handle) {
        struct io_uring_sqe *sqe = GetSqe();
        io_uring_prep_read(sqe, fd, buf, size, 0);
        //io_uring_prep_read_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
        io_uring_sqe_set_data(sqe, handle.address());
    }

    void Write(int fd, char* buf, int size, std::coroutine_handle<> handle) {
        struct io_uring_sqe *sqe = GetSqe();
        io_uring_prep_write(sqe, fd, buf, size, 0);
        //memcpy(Buffer_.data(), buf, size);
        //io_uring_prep_write_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
        io_uring_sqe_set_data(sqe, handle.address());
    }

    void Accept(int fd, struct sockaddr_in* addr, socklen_t* len, std::coroutine_handle<> handle) {
        struct io_uring_sqe *sqe = GetSqe();
        io_uring_prep_accept(sqe, fd, reinterpret_cast<struct sockaddr*>(addr), len, 0);
        io_uring_sqe_set_data(sqe, handle.address());
    }

    void Connect(int fd, struct sockaddr_in* addr, socklen_t len, std::coroutine_handle<> handle) {
        struct io_uring_sqe *sqe = GetSqe();
        io_uring_prep_connect(sqe, fd, reinterpret_cast<struct sockaddr*>(addr), len);
        io_uring_sqe_set_data(sqe, handle.address());
    }

    void Cancel(int fd) {
        struct io_uring_sqe *sqe = GetSqe();
        // io_uring_prep_cancel_fd(sqe, fd, 0);
        io_uring_prep_rw(IORING_OP_ASYNC_CANCEL, sqe, fd, NULL, 0, 0);
        sqe->cancel_flags = (1U << 1);
    }

    int Wait(timespec ts = {10,0}) {
        struct io_uring_cqe *cqe;
        unsigned head;
        int err;

        for (auto& [k, ev] : Events_) {
            assert(!ev.Read);
            assert(!ev.Write);
            Cancel(k);
        }

//        int nfds = 0;
//        int timeout = 1000; // ms
//        epoll_event outEvents[1];

//        while ((nfds =  epoll_wait(EpollFd_, &outEvents[0], 1, timeout)) < 0) {
//            if (errno != EINTR) {
//                throw std::system_error(errno, std::generic_category(), "epoll_wait");
//            }
//        }

//        if (nfds == 1) {
//            eventfd_t v;
//            eventfd_read(RingFd_, &v);
//        }

        struct __kernel_timespec kts = {ts.tv_sec, ts.tv_nsec};
//        if ((err = io_uring_submit_and_wait_timeout(&Ring_, &cqe, 1, &ts, nullptr)) < 0) {
//            if (-err != ETIME) {
//                throw std::system_error(-err, std::generic_category(), "io_uring_wait_cqe_timeout");
//            }
//        }

        if ((err = io_uring_submit(&Ring_) < 0)) {
            throw std::system_error(-err, std::generic_category(), "io_uring_submit");
        }

        if ((err = io_uring_wait_cqe_timeout(&Ring_, &cqe, &kts)) < 0) {
            if (-err != ETIME) {
                throw std::system_error(-err, std::generic_category(), "io_uring_wait_cqe_timeout");
            }
        }

        ReadyHandles_.clear();

        int completed = 0;
        io_uring_for_each_cqe(&Ring_, head, cqe) {
            completed ++;
            void* data = reinterpret_cast<void*>(cqe->user_data);
            if (data != nullptr) {
                Results_.push(cqe->res);
                ReadyHandles_.emplace_back(std::move(std::coroutine_handle<>::from_address(data)));
            }
        }

        io_uring_cq_advance(&Ring_, completed);

        ProcessBacklog();
        ProcessTimers();

        return completed;
    }

    void Poll() {
        auto deadline = Timers_.empty() ? TTime::max() : Timers_.top().Deadline;
        auto ts = GetTimespec(TClock::now(), deadline);
        Wait(ts);
    }

    void Process() {
        for (auto& handle: ReadyHandles_) {
            handle.resume();
        }
        ReadyHandles_.clear();
    }

    int Result() {
        int r = Results_.front();
        Results_.pop();
        return r;
    }

    void Submit() {
        int err;
        if ((err = io_uring_submit(&Ring_) < 0)) {
            throw std::system_error(-err, std::generic_category(), "io_uring_submit");
        }
    }

private:
    io_uring_sqe* GetSqe() {
        io_uring_sqe* r = io_uring_get_sqe(&Ring_);
        if (!r) {
            // TODO: incorrect size for SQE128
            r = &Backlog_.emplace(io_uring_sqe{});
        }
        return r;
    }

    void ProcessBacklog() {
        io_uring_sqe* r = nullptr;
        while (!Backlog_.empty() && (r = io_uring_get_sqe(&Ring_))) {
            *r = Backlog_.front(); Backlog_.pop();
        }
    }

    int RingFd_;
    int EpollFd_;
    struct io_uring Ring_;
    std::queue<int> Results_;
    std::vector<char> Buffer_;
    std::queue<io_uring_sqe> Backlog_;
};

// TODO: XXX
class TUringSocket: public TSocket
{
public:
    TUringSocket(TAddress&& addr, TUring& poller)
        : TSocket(addr, poller)
        , Uring_(&poller)
    { }

    TUringSocket(const TAddress& addr, int fd, TUring& poller)
        : TSocket(addr, fd, poller)
        , Uring_(&poller)
    { }

    TUringSocket() = default;

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

    auto Connect(TTime deadline = TTime::max()) {
        struct TAwaitable {
            bool await_ready() const { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                poller->Connect(fd, &addr, sizeof(addr), h);
                if (deadline != TTime::max()) {
                    poller->AddTimer(fd, deadline, h);
                }
            }

            void await_resume() {
                if (deadline != TTime::max() && poller->RemoveTimer(fd, deadline)) {
                    poller->Cancel(fd);
                    throw std::system_error(std::make_error_code(std::errc::timed_out));
                }
            }

            TUring* poller;
            int fd;
            sockaddr_in addr;
            TTime deadline;
        };
        return TAwaitable{Uring_, Fd_, Addr().Addr(), deadline};
    }

    auto ReadSome(char* buf, size_t size) {
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
                if (ret < 0) {
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

    auto WriteSomeYield(char* buf, size_t size) {
        return WriteSome(buf, size);
    }

    auto ReadSomeYield(char* buf, size_t size) {
        return ReadSome(buf, size);
    }

private:
    TUring* Uring_;
};

} // namespace NNet
