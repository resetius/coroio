#include "socket.hpp"

namespace NNet {

TSocketBase<void>::TSocketBase(TPollerBase& poller, int domain, int type)
    : Poller_(&poller)
    , Fd_(Create(domain, type))
{ }

TSocketBase<void>::TSocketBase(int fd, TPollerBase& poller)
    : Poller_(&poller)
    , Fd_(Setup(fd))
{ }

int TSocketBase<void>::Create(int domain, int type) {
    auto s = socket(domain, type, 0);
    if (s == static_cast<decltype(s)>(-1)) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    return Setup(s);
}

int TSocketBase<void>::Setup(int s) {
    int value;
    socklen_t len = sizeof(value);
    [[maybe_unused]] bool isSocket = false;
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char*) &value, &len) == 0) {
        value = 1;
        isSocket = true;
        // TODO: set for STREAM only
        if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*) &value, len) < 0) {
#ifdef _WIN32
            if (WSAGetLastError() != WSAENOPROTOOPT)
#endif
                throw std::system_error(errno, std::generic_category(), "setsockopt");
        }
    }

#ifdef _WIN32
    // TODO: This code works only with sockets!
    u_long mode = 1;
    if (isSocket && ioctlsocket(s, FIONBIO, &mode) != 0) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "ioctlsocket");
    }
#else
    auto flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl");
    }
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl");
    }
#endif
    return s;
}

TSocket::TSocket(TPollerBase& poller, int domain, int type)
    : TSocketBase(poller, domain, type)
{ }

TSocket::TSocket(const TAddress& addr, int fd, TPollerBase& poller)
    : TSocketBase(fd, poller)
    , RemoteAddr_(addr)
{ }

TSocket::TSocket(TSocket&& other)
{
    *this = std::move(other);
}

TSocket& TSocket::operator=(TSocket&& other) {
    if (this != &other) {
        Close();
        Poller_ = other.Poller_;
        RemoteAddr_ = other.RemoteAddr_;
        LocalAddr_ = other.LocalAddr_;
        Fd_ = other.Fd_;
        other.Fd_ = -1;
    }
    return *this;
}

void TSocket::Bind(const TAddress& addr) {
    if (LocalAddr_.has_value()) {
        throw std::runtime_error("Already bound");
    }
    LocalAddr_ = addr;
    auto [rawaddr, len] = LocalAddr_->RawAddr();
    int optval = 1;
    socklen_t optlen = sizeof(optval);
    if (setsockopt(Fd_, SOL_SOCKET, SO_REUSEADDR, (char*) &optval, optlen) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
    if (bind(Fd_, rawaddr, len) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }
}

void TSocket::Listen(int backlog) {
    if (listen(Fd_, backlog) < 0) {
        throw std::system_error(errno, std::generic_category(), "listen");
    }
}

const std::optional<TAddress>& TSocket::LocalAddr() const {
    return LocalAddr_;
}

const std::optional<TAddress>& TSocket::RemoteAddr() const {
    return RemoteAddr_;
}

int TSocket::Fd() const {
    return Fd_;
}

TFileHandle::TFileHandle(TFileHandle&& other)
{
    *this = std::move(other);
}

TFileHandle& TFileHandle::operator=(TFileHandle&& other) {
    if (this != &other) {
        Close();
        Poller_ = other.Poller_;
        Fd_ = other.Fd_;
        other.Fd_ = -1;
    }
    return *this;
}

} // namespace NNet {
