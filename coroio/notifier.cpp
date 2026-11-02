#include "notifier.h"

#ifndef _WIN32

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

#ifdef __linux__
#include <sys/eventfd.h>
#endif

namespace NNet {

TNotifier::TNotifier(TPollerBase& poller)
    : Poller_(poller)
{
#ifdef __linux__
    ReadFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ReadFd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    WriteFd_ = ReadFd_;
#else
    int fds[2];
    if (pipe(fds) < 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    for (int fd : fds) {
        const int status = fcntl(fd, F_GETFL);
        const int flags = fcntl(fd, F_GETFD);
        if (status < 0 || flags < 0 ||
            fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0 ||
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        {
            const int error = errno;
            close(fds[0]);
            close(fds[1]);
            throw std::system_error(error, std::generic_category(), "fcntl");
        }
    }
    ReadFd_ = fds[0];
    WriteFd_ = fds[1];
#endif
}

TNotifier::~TNotifier() {
    assert(!Waiting_);
    Poller_.RemoveEvent(ReadFd_);
    if (WriteFd_ != ReadFd_) {
        close(WriteFd_);
    }
    close(ReadFd_);
}

void TNotifier::Notify() {
    if (Pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
#ifdef __linux__
    const uint64_t signal = 1;
#else
    const uint8_t signal = 1;
#endif
    for (;;) {
        const auto size = write(WriteFd_, &signal, sizeof(signal));
        if (size == static_cast<ssize_t>(sizeof(signal))) {
            return;
        }
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0 && errno == EAGAIN) {
            return;
        }
        const int error = size < 0 ? errno : EIO;
        Pending_.store(false, std::memory_order_release);
        throw std::system_error(error, std::generic_category(), "notifier write");
    }
}

bool TNotifier::TryRead() {
#ifdef __linux__
    uint64_t signal;
#else
    uint8_t signal;
#endif
    for (;;) {
        const auto size = read(ReadFd_, &signal, sizeof(signal));
        if (size == static_cast<ssize_t>(sizeof(signal))) {
            return true;
        }
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0 && errno == EAGAIN) {
            return false;
        }
        throw std::system_error(size < 0 ? errno : EIO,
            std::generic_category(), "notifier read");
    }
}

TFuture<void> TNotifier::Wait() {
    if (Waiting_) {
        throw std::logic_error("notifier already has a listener");
    }
    Waiting_ = true;
    struct TWaitGuard {
        bool& Waiting;
        ~TWaitGuard() {
            Waiting = false;
        }
    } guard{Waiting_};

    struct TReadAwaiter {
        TNotifier& Owner;
        bool Ready = false;
        bool Suspended = false;

        ~TReadAwaiter() {
            if (Suspended) {
                Owner.Poller_.RemoveEvent(Owner.ReadFd_);
            }
        }

        bool await_ready() {
            return Ready = Owner.TryRead();
        }

        void await_suspend(THandle handle) {
            Owner.Poller_.AddRead(Owner.ReadFd_, handle);
            Suspended = true;
        }

        bool await_resume() {
            Suspended = false;
            return Ready || Owner.TryRead();
        }
    };

    while (!(co_await TReadAwaiter{*this})) {
    }
    // Re-arm before the caller drains its work, so concurrent work can signal again.
    Pending_.exchange(false, std::memory_order_acq_rel);
}

} // namespace NNet

#endif // _WIN32
