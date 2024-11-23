#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <iostream>

namespace {

int map_winsock_error_to_errno(int winsock_error) {
    switch (winsock_error) {
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAECONNRESET: return ECONNRESET;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAEINTR: return EINTR;
        case WSAEINVAL: return EINVAL;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAENOTSOCK: return ENOTSOCK;
        default: return errno;
    }
}

} // namespace

int process_errno() {
    return errno == 0 ? errno = map_winsock_error_to_errno(WSAGetLastError()) : errno;
}
#else
int process_errno()
{
    return errno;
}
#endif
