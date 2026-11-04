#ifdef __linux__
#include "uring.hpp"

#ifdef HAVE_URING

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
    io_uring_prep_read(sqe, fd, buf, size, -1);
    //io_uring_prep_read_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Write(int fd, const void* buf, int size, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_write(sqe, fd, buf, size, -1);
    //memcpy(Buffer_.data(), buf, size);
    //io_uring_prep_write_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Recv(int fd, void* buf, int size, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_recv(sqe, fd, buf, size, 0);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Send(int fd, const void* buf, int size, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_send(sqe, fd, buf, size, 0);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Accept(int fd, sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_accept(sqe, fd, addr, len, SOCK_CLOEXEC);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle) {
    struct io_uring_sqe *sqe = GetSqe();
    io_uring_prep_connect(sqe, fd, addr, len);
    io_uring_sqe_set_data(sqe, handle.address());
}

void TUring::Cancel(int fd) {
    struct io_uring_sqe *sqe = GetSqe();
    // io_uring_prep_cancel_fd(sqe, fd, 0);
    io_uring_prep_rw(IORING_OP_ASYNC_CANCEL, sqe, fd, nullptr, 0, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    sqe->cancel_flags = (1U << 1);
}

int TUring::Cancel(std::coroutine_handle<> h) {
    for (size_t i = 0; i < ReadyEvents_.size(); i++) {
        if (ReadyEvents_[i].Handle == h) {
            ReadyEvents_[i].Handle = {};
            return Results_[i];
        }
    }
    for (auto& [handle, result] : Reaped_) {
        if (handle == h) {
            handle = {};
            return result;
        }
    }

    // Not submitted yet: the kernel has not seen the operation, so it can be dropped as is.
    auto data = reinterpret_cast<__u64>(h.address());
    for (unsigned i = *Ring_.sq.khead; i != Ring_.sq.sqe_tail; i++) {
        struct io_uring_sqe *sqe = &Ring_.sq.sqes[i & *Ring_.sq.kring_mask];
        if (sqe->user_data == data) {
            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data(sqe, nullptr);
            return -ECANCELED;
        }
    }

    // Called from destructors: errors that persist end the wait instead of throwing.
    auto failed = [](int err) {
        return err < 0 && err != -EINTR && err != -EAGAIN && err != -EBUSY;
    };
    int result = -ECANCELED;
    struct io_uring_sqe *sqe;
    while (!(sqe = io_uring_get_sqe(&Ring_))) {
        // The SQ is full. Submitting it fails while the CQ overflows, so drain that too.
        int err = io_uring_submit(&Ring_);
        if (Reap(h, &result) || failed(err)) {
            return result;
        }
    }
    io_uring_prep_cancel(sqe, h.address(), 0);
    io_uring_sqe_set_data(sqe, nullptr);

    // The kernel may use the operation's buffers until its completion is posted,
    // so wait for it. Other completions are kept for the next Wait().
    while (!Reap(h, &result)) {
        if (failed(io_uring_submit_and_wait(&Ring_, 1))) {
            break;
        }
    }
    return result;
}

bool TUring::Reap(std::coroutine_handle<> h, int* result) {
    struct io_uring_cqe *cqe;
    unsigned head;
    int completed = 0;
    bool found = false;
    io_uring_for_each_cqe(&Ring_, head, cqe) {
        completed ++;
        void* data = reinterpret_cast<void*>(cqe->user_data);
        if (data == h.address()) {
            found = true;
            *result = cqe->res;
        } else if (data != nullptr) {
            Reaped_.emplace_back(std::coroutine_handle<>::from_address(data), cqe->res);
        }
    }
    io_uring_cq_advance(&Ring_, completed);
    return found;
}

void TUring::Register(int) {

}

int TUring::Wait(timespec ts) {
    struct io_uring_cqe *cqe;
    unsigned head;
    int err;

    for (auto& ev : Changes_) {
        assert(ev.Type == (TEvent::READ|TEvent::WRITE|TEvent::RHUP));
        assert(!ev.Handle);
        Cancel(ev.Fd);
    }

    // Collected by the previous call, but not delivered: WakeupReadyHandles() was not called.
    for (size_t i = 0; i < ReadyEvents_.size(); i++) {
        if (ReadyEvents_[i].Handle) {
            Reaped_.emplace_back(ReadyEvents_[i].Handle, Results_[i]);
        }
    }

    Reset();
    Results_.clear();
    NextResult_ = 0;

    for (auto [h, result] : Reaped_) {
        if (h) {
            Results_.push_back(result);
            ReadyEvents_.emplace_back(TEvent{-1, 0, h});
        }
    }
    if (!Reaped_.empty()) {
        Reaped_.clear();
        ts = {0, 0}; // completions are ready already, do not block
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
    if ((err = io_uring_submit_and_wait_timeout(&Ring_, &cqe, 1, &kts, nullptr)) < 0) {
        if (-err != ETIME) {
            throw std::system_error(-err, std::generic_category(), "io_uring_wait_cqe_timeout");
        }
    }

    int completed = 0;
    io_uring_for_each_cqe(&Ring_, head, cqe) {
        completed ++;
        void* data = reinterpret_cast<void*>(cqe->user_data);
        if (data != nullptr) {
            Results_.push_back(cqe->res);
            ReadyEvents_.emplace_back(TEvent{-1, 0, std::coroutine_handle<>::from_address(data)});
        }
    }

    io_uring_cq_advance(&Ring_, completed);

    ProcessTimers();

    return completed;
}

int TUring::Result() {
    return Results_[NextResult_++];
}

void TUring::WakeupReadyHandles() {
    for (size_t i = 0; i < ReadyEvents_.size(); i++) {
        // Cleared before resuming, so that Cancel() finds only completions not delivered yet.
        if (auto h = std::exchange(ReadyEvents_[i].Handle, {})) {
            NextResult_ = i;
            h.resume();
        }
    }
    ReadyEvents_.clear();
}

void TUring::Submit() {
    int err;
    if ((err = io_uring_submit(&Ring_) < 0)) {
        throw std::system_error(-err, std::generic_category(), "io_uring_submit");
    }
}

} // namespace NNet

#endif // HAVE_URING

#endif // __linux__
