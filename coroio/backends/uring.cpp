#ifdef __linux__
#include "uring.hpp"
#include <coroio/notifier.hpp>

#include <algorithm>
#include <cstring>

#ifdef HAVE_URING

namespace NNet {

std::unique_ptr<INotifier> TUring::CreateNotifier() {
    return std::make_unique<TNotifier<TUring>>(*this);
}

namespace {

constexpr int BufferGroup = 0;

// The user_data of a buffered read: odd, unlike a TOperation address.
uint64_t Tag(int fd, uint32_t gen) {
    return (static_cast<uint64_t>(gen) << 32) | (static_cast<uint64_t>(fd) << 1) | 1;
}

bool IsTag(uint64_t data) {
    return data & 1;
}

int TagFd(uint64_t data) {
    return static_cast<int>(static_cast<uint32_t>(data) >> 1);
}

uint32_t TagGen(uint64_t data) {
    return static_cast<uint32_t>(data >> 32);
}

} // namespace

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
    params.flags = IORING_SETUP_CQSIZE;
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
    io_uring_submit(&Ring_); // best effort: closes prepared by a Wait() that threw
    if (BufRing_) {
        // Before the exit, which cancels the multishot reads later: none may pick a buffer of the freed pool.
        io_uring_free_buf_ring(&Ring_, BufRing_, Buffers_.Config().Count, BufferGroup);
        Buffers_.SetOwner(nullptr);
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
        if (IsTag(cqe->user_data)) {
            OnBuffered(cqe, true);
            continue;
        }
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

    for (auto& ev : Changes_) {
        assert(!ev.Type || (ev.Type & ~TEvent::CLOSE) == (TEvent::READ|TEvent::WRITE|TEvent::RHUP));
        assert(!ev.Handle);
        if (!ev.Type) {
            continue; // a close already in the ring
        }
        if (ev.Type & TEvent::CLOSE) {
            ResetBuffered(ev.Fd);
            PrepClose(ev);
        } else {
            Cancel(ev.Fd);
        }
    }
    PrepBuffered();

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
    io_uring_for_each_cqe(&Ring_, head, cqe) {
        completed ++;
        if (IsTag(cqe->user_data)) {
            OnBuffered(cqe, false);
        } else if (auto* op = static_cast<TOperation*>(io_uring_cqe_get_data(cqe))) {
            Done(*op);
            Completions_.Collect(op, cqe->res);
        }
    }

    io_uring_cq_advance(&Ring_, completed);

    ProcessTimers();

    return completed;
}

void TUring::WakeupReadyHandles() {
    Completions_.Wakeup();
}

bool TUring::TakeBuffered(int fd, TReadBuffer& lease) {
    if (fd < 0) {
        throw std::system_error(EBADF, std::generic_category(), "read");
    }
    if (fd >= static_cast<int>(Buffered_.size())) {
        return false;
    }
    auto& state = Buffered_[fd];
    if (state.Front.Size) {
        lease = Buffers_.Commit(state.Front.Id, state.Front.Pos + state.Front.Size);
        lease.Consume(state.Front.Pos);
        state.Refill();
        return true;
    }
    if (state.Error) {
        throw std::system_error(std::exchange(state.Error, 0), std::generic_category(), "read");
    }
    if (state.Eof) {
        lease = {};
        return true;
    }
    return false;
}

int TUring::ReadBuffered(int fd, void* buf, size_t size) {
    if (fd < 0) {
        throw std::system_error(EBADF, std::generic_category(), "read");
    }
    if (fd >= static_cast<int>(Buffered_.size())) {
        return -1;
    }
    auto& state = Buffered_[fd];
    size_t copied = 0;
    while (copied < size && state.Front.Size) {
        auto& front = state.Front;
        size_t n = std::min<size_t>(size - copied, front.Size);
        memcpy(static_cast<char*>(buf) + copied, Buffers_.Buffer(front.Id) + front.Pos, n);
        front.Pos += static_cast<uint32_t>(n);
        front.Size -= static_cast<uint32_t>(n);
        copied += n;
        if (!front.Size) {
            RingAdd(front.Id);
            state.Refill();
        }
    }
    if (copied || !size) {
        return static_cast<int>(copied);
    }
    if (state.Error) {
        throw std::system_error(std::exchange(state.Error, 0), std::generic_category(), "read");
    }
    return state.Eof ? 0 : -1;
}

void TUring::WaitBuffered(int fd, bool file, TOperation& op) {
    try {
        if (fd >= static_cast<int>(Buffered_.size())) {
            Buffered_.resize(fd + 1);
        }
        if (fd >= static_cast<int>(Operations_.size())) {
            Operations_.resize(fd + 1);
        }
        SetupBuffers();
        auto& state = Buffered_[fd];
        assert(!state.Waiter); // one reader per descriptor
        state.File = file;
        if (!state.Armed) {
            Arm(fd, state);
        }
        state.Waiter = &op;
    } catch (...) {
        op.Handle = {};
        throw;
    }
}

void TUring::DropWaiter(int fd, TOperation& op) {
    op.Handle = {};
    if (fd >= 0 && fd < static_cast<int>(Buffered_.size()) && Buffered_[fd].Waiter == &op) {
        Buffered_[fd].Waiter = nullptr;
    } else {
        Completions_.Drop(op); // woken already
    }
}

void TUring::SetupBuffers() {
    if (BufRing_) {
        return;
    }
    Buffers_.Allocate();
    const auto& config = Buffers_.Config();
    int err = 0;
    BufRing_ = io_uring_setup_buf_ring(&Ring_, config.Count, BufferGroup, 0, &err);
    if (!BufRing_) {
        throw std::system_error(-err, std::generic_category(), "io_uring_setup_buf_ring");
    }
    int mask = io_uring_buf_ring_mask(config.Count);
    for (uint32_t id = 0; id < config.Count; id++) {
        io_uring_buf_ring_add(BufRing_, Buffers_.Buffer(id), config.BufferSize, id, mask, id);
    }
    io_uring_buf_ring_advance(BufRing_, config.Count);
    Buffers_.SetOwner(this);
    Returned_.reserve(config.Count);
    InRing_ = config.Count;
}

void TUring::RingAdd(uint16_t id) {
    Returned_.push_back(id);
}

void TUring::FlushReturned() {
    if (Returned_.empty()) {
        return;
    }
    // One tail update per Wait(); a read that finds the ring empty meanwhile waits for it, as after -ENOBUFS.
    const auto& config = Buffers_.Config();
    int mask = io_uring_buf_ring_mask(config.Count);
    for (size_t i = 0; i < Returned_.size(); i++) {
        io_uring_buf_ring_add(BufRing_, Buffers_.Buffer(Returned_[i]), config.BufferSize, Returned_[i], mask, i);
    }
    io_uring_buf_ring_advance(BufRing_, Returned_.size());
    InRing_ += Returned_.size();
    for (size_t i = 0; i < Returned_.size() && !Starved_.empty(); i++) {
        Rearm_.push_back(Starved_.back());
        Starved_.pop_back();
    }
    Returned_.clear();
}

void TUring::Release(uint16_t id) {
    RingAdd(id);
}

void TUring::Arm(int fd, TBufferedFd& state) {
    io_uring_sqe* sqe = GetSqe();
    if (state.Single) {
        io_uring_prep_read(sqe, fd, nullptr, Buffers_.Config().BufferSize, -1);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = BufferGroup;
    } else if (state.File) {
        io_uring_prep_read_multishot(sqe, fd, 0, -1, BufferGroup);
    } else {
        io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = BufferGroup;
    }
    io_uring_sqe_set_data64(sqe, Tag(fd, state.Gen));
    Operations_[fd]++; // so that a close cancels it
    state.Armed = true;
    state.Starved = false;
    state.Throttled = false;
}

void TUring::OnBuffered(const io_uring_cqe* cqe, bool reaped) {
    int fd = TagFd(cqe->user_data);
    bool more = cqe->flags & IORING_CQE_F_MORE;
    if (!more) {
        assert(Operations_[fd] > 0);
        Operations_[fd]--;
    }
    auto& state = Buffered_[fd];
    bool own = TagGen(cqe->user_data) == state.Gen;
    if (cqe->flags & IORING_CQE_F_BUFFER) {
        InRing_--;
        auto id = static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
        if (own && cqe->res > 0) {
            state.Push({static_cast<uint32_t>(cqe->res), 0, id});
        } else {
            RingAdd(id);
        }
    }
    if (!own) {
        return;
    }
    if (!more) {
        state.Armed = false;
    }
    if (cqe->res == 0) {
        state.Eof = true;
    } else if (cqe->res == -ENOBUFS) {
        state.Starved = true;
    } else if (cqe->res == -EBADFD && state.File && !state.Single) {
        state.Single = true; // a regular file: read it one buffer at a time
    } else if (cqe->res < 0 && cqe->res != -ECANCELED) {
        state.Error = -cqe->res;
    }
    if (state.Waiter && (state.Pending() || state.Eof || state.Error)) {
        Wake(state, 0, reaped);
    } else if (state.Waiter && !state.Armed) {
        // Starved: wait for a buffer, unless some came back since the kernel looked.
        (state.Starved && !InRing_ ? Starved_ : Rearm_).emplace_back(fd, state.Gen);
    }
    if (state.Armed && !state.Throttled && state.Pending() >= Buffers_.Config().PerSocketLimit) {
        // Leaves the rest in the socket, so one fast sender does not take the whole ring.
        state.Throttled = true;
        Throttle_.emplace_back(fd, state.Gen);
    }
}

void TUring::Wake(TBufferedFd& state, int result, bool reaped) {
    auto* op = std::exchange(state.Waiter, nullptr);
    // The frame is resumed after the completions are taken: start loading it now.
    __builtin_prefetch(op->Handle.address());
    if (reaped) {
        Completions_.Reap(op, result);
    } else {
        Completions_.Collect(op, result);
    }
}

void TUring::PrepBuffered() {
    FlushReturned();
    for (auto [fd, gen] : Throttle_) {
        if (Buffered_[fd].Gen == gen && Buffered_[fd].Armed) {
            io_uring_sqe* sqe = GetSqe();
            io_uring_prep_cancel64(sqe, Tag(fd, gen), 0);
            io_uring_sqe_set_data(sqe, nullptr);
        }
    }
    Throttle_.clear();
    for (auto [fd, gen] : Rearm_) {
        auto& state = Buffered_[fd];
        if (state.Gen == gen && !state.Armed && !state.Eof && !state.Error) {
            Arm(fd, state);
        }
    }
    Rearm_.clear();
}

void TUring::ResetBuffered(int fd) {
    if (fd >= static_cast<int>(Buffered_.size())) {
        return;
    }
    auto& state = Buffered_[fd];
    if (state.Waiter) {
        Wake(state, -ECANCELED, false);
    }
    for (; state.Front.Size; state.Refill()) {
        RingAdd(state.Front.Id);
    }
    // The read in the kernel is cancelled by the close; its completions carry the old generation.
    TBufferedFd next;
    next.Gen = state.Gen + 1;
    state = std::move(next);
}

void TUring::Submit() {
    int err;
    if ((err = io_uring_submit(&Ring_)) < 0) {
        throw std::system_error(-err, std::generic_category(), "io_uring_submit");
    }
}

} // namespace NNet

#endif // HAVE_URING

#endif // __linux__
