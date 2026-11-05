#include "notifier.hpp"

#ifndef _WIN32

#include <fcntl.h>

#ifdef __linux__
#include <sys/eventfd.h>
#endif

namespace NNet::NDetail {

std::array<int, 2> MakeNotifierFds() {
#ifdef __linux__
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    return {fd, fd};
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
    return {fds[0], fds[1]};
#endif
}

void WriteNotifierSignal(int fd) {
#ifdef __linux__
    const uint64_t signal = 1;
#else
    const uint8_t signal = 1;
#endif
    for (;;) {
        const auto size = write(fd, &signal, sizeof(signal));
        if (size == static_cast<ssize_t>(sizeof(signal))) {
            return;
        }
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0 && errno == EAGAIN) {
            return;
        }
        throw std::system_error(size < 0 ? errno : EIO,
            std::generic_category(), "notifier write");
    }
}

} // namespace NNet::NDetail

#endif // _WIN32
