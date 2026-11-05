#pragma once

#include <cerrno>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace NNet {

/// The error of Accept() concerns only the connection being accepted, or no connection was there: accept again.
inline bool IsTransientAcceptError(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAECONNRESET;
#else
    switch (err) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINTR:
    case ECONNABORTED:
    case EPROTO:
#ifdef __linux__
    // Network errors pending on the new connection, see accept(2). EOPNOTSUPP and
    // ENOPROTOOPT are left out: they also come from a socket that cannot accept, e.g.
    // UDP, on every call.
    case ENETDOWN: case EHOSTDOWN: case ENONET: case EHOSTUNREACH: case ENETUNREACH:
#endif
        return true;
    default:
        return false;
    }
#endif
}

} // namespace NNet
