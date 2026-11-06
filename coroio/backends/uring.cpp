#ifdef __linux__
#include "uring.hpp"

#include <algorithm>

#ifdef HAVE_URING

namespace NNet {

// The kernel sizes the hash that cancels by user_data search from the CQ: 16 buckets for the
// default 512 entries, so such a cancel walked ~1/32 of the pending operations; 8192 gives 256, the most.
TUring::TUring(int queueSize)
    : TUring(queueSize, 8192)
{ }

TUring::TUring(int queueSize, int completionQueueSize)
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
    int entries = std::max(queueSize, 2); // a close takes two SQEs
    io_uring_params params = {};
    // Completions of operations that waited are posted in our next io_uring_enter, in a batch,
    // instead of by task work signalled to the thread. Disabled until Enable(), so that the
    // ring is bound to the thread that runs it, not to the one that constructs it.
    params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN
        | IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_R_DISABLED;
    params.cq_entries = std::max(2 * entries, completionQueueSize);
    if ((err = io_uring_queue_init_params(entries, &Ring_, &params)) < 0) {
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
    // Best effort: closes prepared by a Wait() that threw. They run as deferred task work,
    // which the ring's exit would cancel instead. A ring never enabled has nothing in the kernel.
    if (Enabled_) {
        io_uring_submit_and_get_events(&Ring_);
        while (RunTaskWork()) { }
    }
    io_uring_queue_exit(&Ring_);
    close(RingFd_);
    close(EpollFd_);
}

void TUring::Read(int fd, void* buf, int size, TOperation& op) {
    struct io_uring_sqe *sqe = GetSqe(op, fd);
    io_uring_prep_read(sqe, fd, buf, size, -1);
    //io_uring_prep_read_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
    SetData(sqe, op);
}

void TUring::Write(int fd, const void* buf, int size, TOperation& op) {
    struct io_uring_sqe *sqe = GetSqe(op, fd);
    io_uring_prep_write(sqe, fd, buf, size, -1);
    //memcpy(Buffer_.data(), buf, size);
    //io_uring_prep_write_fixed(sqe, fd, Buffer_.data(), size, 0, 0);
    SetData(sqe, op);
}

void TUring::Recv(int fd, void* buf, int size, TOperation& op) {
    struct io_uring_sqe *sqe = GetSqe(op, fd);
    io_uring_prep_recv(sqe, fd, buf, size, 0);
    SetData(sqe, op);
}

void TUring::Send(int fd, const void* buf, int size, TOperation& op) {
    struct io_uring_sqe *sqe = GetSqe(op, fd);
    io_uring_prep_send(sqe, fd, buf, size, 0);
    SetData(sqe, op);
}

void TUring::Accept(int fd, sockaddr* addr, socklen_t* len, TOperation& op) {
    struct io_uring_sqe *sqe = GetSqe(op, fd);
    io_uring_prep_accept(sqe, fd, addr, len, SOCK_CLOEXEC);
    SetData(sqe, op);
}

void TUring::Connect(int fd, const sockaddr* addr, socklen_t len, TOperation& op) {
    struct io_uring_sqe *sqe = GetSqe(op, fd);
    io_uring_prep_connect(sqe, fd, addr, len);
    SetData(sqe, op);
}

void TUring::Cancel(int fd) {
    if (!HasOperations(fd)) {
        return;
    }
    struct io_uring_sqe *sqe = GetSqe();
    // io_uring_prep_cancel_fd(sqe, fd, 0);
    io_uring_prep_rw(IORING_OP_ASYNC_CANCEL, sqe, fd, nullptr, 0, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    sqe->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
}

void TUring::PrepClose(TEvent& change) {
    if (!HasOperations(change.Fd)) {
        // Nothing to cancel: the kernel looks up a cancel by fd among all pending operations.
        io_uring_sqe* sqe = GetSqe();
        io_uring_prep_close(sqe, change.Fd);
        io_uring_sqe_set_data(sqe, nullptr);
        change.Type = 0;
        return;
    }
    if (io_uring_sq_space_left(&Ring_) < 2) {
        Submit(); // never split the linked pair
    }
    // Cancel by fd (ALL: a reader and a writer) before the close releases the number.
    // HARDLINK: the close must run even if the cancel fails, e.g. -EINVAL before 5.19.
    io_uring_sqe* sqe = io_uring_get_sqe(&Ring_);
    io_uring_prep_rw(IORING_OP_ASYNC_CANCEL, sqe, change.Fd, nullptr, 0, 0);
    sqe->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
    sqe->flags |= IOSQE_IO_HARDLINK;
    io_uring_sqe_set_data(sqe, nullptr);
    sqe = io_uring_get_sqe(&Ring_);
    io_uring_prep_close(sqe, change.Fd);
    io_uring_sqe_set_data(sqe, nullptr);
    // Consumed: a Wait() rerun after a failed Submit() must not cancel the number again.
    change.Type = 0;
}

void TUring::SetData(io_uring_sqe* sqe, TOperation& op) {
    op.Index = Ring_.sq.sqe_tail - 1;
    io_uring_sqe_set_data(sqe, &op);
    op.Fd = sqe->fd;
    if (op.Fd >= 0) { // not e.g. an accept on a closed socket, which fails with -EBADF
        Operations_[op.Fd]++;
    }
}

void TUring::Done(TOperation& op) {
    if (op.Fd >= 0) {
        assert(Operations_[op.Fd] > 0);
        Operations_[op.Fd]--;
    }
}

bool TUring::HasOperations(int fd) const {
    return fd >= 0 && fd < static_cast<int>(Operations_.size()) && Operations_[fd] > 0;
}

int TUring::Cancel(TOperation& op) {
    op.Handle = {};
    if (Completions_.Drop(op)) {
        return op.Result;
    }

    // Not submitted yet: the kernel has not seen the operation, so it can be dropped as is.
    // Without SQPOLL the kernel reads SQEs only in io_uring_enter, so this holds from *khead;
    // with SQPOLL only sqe_head..sqe_tail could be rewritten. The position comes round again
    // after 2^32 SQEs, so the entry is checked to be still the operation's.
    unsigned head = *Ring_.sq.khead;
    if (op.Index - head < Ring_.sq.sqe_tail - head) {
        struct io_uring_sqe *sqe = &Ring_.sq.sqes[op.Index & *Ring_.sq.kring_mask];
        if (sqe->user_data == reinterpret_cast<__u64>(&op)) {
            Done(op);
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
        if (Reap(op, &result) || failed(err)) {
            return result;
        }
    }
    io_uring_prep_cancel(sqe, &op, 0);
    io_uring_sqe_set_data(sqe, nullptr);

    // The kernel may use the operation's buffers until its completion is posted,
    // so wait for it. Other completions are kept for the next Wait().
    while (!Reap(op, &result)) {
        if (failed(io_uring_submit_and_wait(&Ring_, 1))) {
            break;
        }
    }
    return result;
}

bool TUring::Reap(TOperation& op, int* result) {
    struct io_uring_cqe *cqe;
    unsigned head;
    int completed = 0;
    bool found = false;
    io_uring_for_each_cqe(&Ring_, head, cqe) {
        completed ++;
        auto* other = static_cast<TOperation*>(io_uring_cqe_get_data(cqe));
        if (other == &op) {
            Done(op);
            found = true;
            *result = cqe->res;
        } else if (other != nullptr) {
            Done(*other);
            Completions_.Reap(other, cqe->res);
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

    Enable();
    for (auto& ev : Changes_) {
        assert(!ev.Type || (ev.Type & ~TEvent::CLOSE) == (TEvent::READ|TEvent::WRITE|TEvent::RHUP));
        assert(!ev.Handle);
        if (!ev.Type) {
            continue; // a close already in the ring
        }
        if (ev.Type & TEvent::CLOSE) {
            PrepClose(ev);
        } else {
            Cancel(ev.Fd);
        }
    }

    Reset();
    if (Completions_.Carry()) {
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
    int reaped;
    do {
        reaped = 0;
        io_uring_for_each_cqe(&Ring_, head, cqe) {
            reaped ++;
            if (auto* op = static_cast<TOperation*>(io_uring_cqe_get_data(cqe))) {
                Done(*op);
                Completions_.Collect(op, cqe->res);
            }
        }
        io_uring_cq_advance(&Ring_, reaped);
        completed += reaped;
        // A wait runs a bounded batch of the deferred work: run the rest, e.g. linked closes.
    } while (RunTaskWork());

    ProcessTimers();

    return completed;
}

void TUring::WakeupReadyHandles() {
    Completions_.Wakeup();
}

bool TUring::RunTaskWork() {
    return (IO_URING_READ_ONCE(*Ring_.sq.kflags) & IORING_SQ_TASKRUN) && io_uring_get_events(&Ring_) >= 0;
}

void TUring::Enable() {
    if (Enabled_) {
        return;
    }
    int err;
    if ((err = io_uring_enable_rings(&Ring_)) < 0) {
        throw std::system_error(-err, std::generic_category(), "io_uring_enable_rings");
    }
    Enabled_ = true;
}

void TUring::Submit() {
    Enable();
    int err;
    if ((err = io_uring_submit(&Ring_)) < 0) {
        throw std::system_error(-err, std::generic_category(), "io_uring_submit");
    }
}

} // namespace NNet

#endif // HAVE_URING

#endif // __linux__
