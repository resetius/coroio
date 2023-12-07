#ifdef __linux__
#include "uring.hpp"

namespace NNet {

TUring::TUring(int queueSize)
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

    utsname buffer;
    if (uname(&buffer) != 0) {
        throw std::system_error(errno, std::generic_category(), "uname");
    }
    int ver[3];
    const char* sep = ".";
    char* p = buffer.release;
    KernelStr_ = buffer.release;

    int i = 0;
    for (p = strtok(p, sep); p && i < 3; p = strtok(nullptr, sep)) {
        ver[i++] = atoi(p);
    }

    Kernel_ = std::make_tuple(ver[0], ver[1], ver[2]);

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

TUring::~TUring() {
    io_uring_queue_exit(&Ring_);
    close(RingFd_);
    close(EpollFd_);
}

void TUring::Read(int fd, void* buf, int size, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_read(sqe, fd, buf, size, 0);
    //io_uring_prep_read_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Write(int fd, const void* buf, int size, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_write(sqe, fd, buf, size, 0);
    //memcpy(Buffer_.data(), buf, size);
    //io_uring_prep_write_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Accept(int fd, struct sockaddr_in* addr, socklen_t* len, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_accept(sqe, fd, reinterpret_cast<struct sockaddr*>(addr), len, 0);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Connect(int fd, struct sockaddr_in* addr, socklen_t len, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_connect(sqe, fd, reinterpret_cast<struct sockaddr*>(addr), len);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Cancel(int fd) {
    struct io_uring_sqe *sqe = GetSqe();
    // io_uring_prep_cancel_fd(sqe, fd, 0);
    io_uring_prep_rw(IORING_OP_ASYNC_CANCEL, sqe, fd, nullptr, 0, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    sqe->cancel_flags = (1U << 1);
}

void TUring::Cancel(std::coroutine_handle<> h) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_cancel(sqe, h.address(), 0);
}

int TUring::Wait(timespec ts) {
    struct io_uring_cqe *cqe;
    unsigned head;
    int err;

    for (auto& ev : Changes_) {
        assert(ev.Type == (TEvent::READ|TEvent::WRITE));
        assert(!ev.Handle);
        Cancel(ev.Fd);
    }

    Reset();

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

    Submit();

    if ((err = io_uring_wait_cqe_timeout(&Ring_, &cqe, &kts)) < 0) {
        if (-err != ETIME) {
            throw std::system_error(-err, std::generic_category(), "io_uring_wait_cqe_timeout");
        }
    }

    assert(Results_.empty());

    int completed = 0;
    io_uring_for_each_cqe(&Ring_, head, cqe) {
        completed ++;
        void* data = reinterpret_cast<void*>(cqe->user_data);
        if (data != nullptr) {
            Results_.push(cqe->res);
            ReadyEvents_.emplace_back(TEvent{-1, 0, std::coroutine_handle<>::from_address(data)});
        }
    }

    io_uring_cq_advance(&Ring_, completed);

    ProcessTimers();

    return completed;
}

int TUring::Result() {
    int r = Results_.front();
    Results_.pop();
    return r;
}

void TUring::Submit() {
    int err;
    if ((err = io_uring_submit(&Ring_) < 0)) {
        throw std::system_error(-err, std::generic_category(), "io_uring_submit");
    }
}

std::tuple<int, int, int> TUring::Kernel() const {
    return Kernel_;
}

const std::string& TUring::KernelStr() const {
    return KernelStr_;
}

} // namespace NNet

#endif // __linux__
