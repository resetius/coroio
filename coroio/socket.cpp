#include "socket.hpp"

#ifndef _WIN32
#include <signal.h>
#endif

namespace NNet {

TInitializer::TInitializer() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#else
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with error: " + std::to_string(result));
    }
#endif
}

#ifdef _WIN32
TInitializer::~TInitializer() {
    WSACleanup();
}
#endif

TAddress::TAddress(const std::string& addr, int port)
{
    sockaddr_in addr4 = {};
    sockaddr_in6 addr6 = {};
    if (inet_pton(AF_INET, addr.c_str(), &(addr4.sin_addr)) == 1) {
        addr4.sin_port = htons(port);
        addr4.sin_family = AF_INET;
        Addr_ = std::move(addr4);
    } else if (inet_pton(AF_INET6, addr.c_str(), &(addr6.sin6_addr)) == 1) {
        addr6.sin6_port = htons(port);
        addr6.sin6_family = AF_INET6;
        Addr_ = std::move(addr6);
    } else {
        throw std::runtime_error("Cannot parse address: '" + addr + "'");
    }
}

TAddress::TAddress(sockaddr_in addr)
    : Addr_(addr)
{ }

TAddress::TAddress(sockaddr_in6 addr)
    : Addr_(addr)
{ }

TAddress::TAddress(sockaddr* addr, socklen_t len) {
    if (len == sizeof(sockaddr_in)) {
        sockaddr_in addr4; memcpy(&addr4, addr, sizeof(addr4));
        Addr_ = addr4;
    } else if (len == sizeof(sockaddr_in6)) {
        sockaddr_in6 addr6; memcpy(&addr6, addr, sizeof(addr6));
        Addr_ = addr6;
    } else {
        throw std::runtime_error("Bad address size: " + std::to_string(len));
    }
}

int TAddress::Domain() const {
    if (std::get_if<sockaddr_in>(&Addr_)) {
        return PF_INET;
    } else if (std::get_if<sockaddr_in6>(&Addr_)) {
        return PF_INET6;
    } else {
        return 0;
    }
}

TAddress TAddress::WithPort(int port) const {
    if (const auto* val = std::get_if<sockaddr_in>(&Addr_)) {
        auto v = *val; v.sin_port = htons(port);
        return TAddress{v};
    } else if (const auto* val = std::get_if<sockaddr_in6>(&Addr_)) {
        auto v = *val; v.sin6_port = htons(port);
        return TAddress{v};
    } else {
        throw std::runtime_error("Unknown address type");
    }
}

const std::variant<sockaddr_in, sockaddr_in6>& TAddress::Addr() const { return Addr_; }
std::pair<const sockaddr*, int> TAddress::RawAddr() const {
    if (const auto* val = std::get_if<sockaddr_in>(&Addr_)) {
        return {reinterpret_cast<const sockaddr*>(val), sizeof(sockaddr_in)};
    } else if (const auto* val = std::get_if<sockaddr_in6>(&Addr_)) {
        return {reinterpret_cast<const sockaddr*>(val), sizeof(sockaddr_in6)};
    } else {
        throw std::runtime_error("Empty variant");
    }
}

bool TAddress::operator == (const TAddress& other) const {
    return memcmp(&Addr_, &other.Addr_, sizeof(Addr_)) == 0;
}

std::string TAddress::ToString() const {
    char buf[1024];
    if (const auto* val = std::get_if<sockaddr_in>(&Addr_)) {
        auto* r = inet_ntop(AF_INET, &val->sin_addr, buf, sizeof(buf));
        if (r) {
            return std::string(r) + ":" + std::to_string(ntohs(val->sin_port));
        }
    } else if (const auto* val = std::get_if<sockaddr_in6>(&Addr_)) {
        auto* r = inet_ntop(AF_INET6, &val->sin6_addr, buf, sizeof(buf));
        if (r) {
            return "[" + std::string(r) + "]:" + std::to_string(ntohs(val->sin6_port));
        }
    }

    return "";
}

TSocketOps::TSocketOps(TPollerBase& poller, int domain, int type)
    : Poller_(&poller)
    , Fd_(Create(domain, type))
{ }

TSocketOps::TSocketOps(int fd, TPollerBase& poller)
    : Poller_(&poller)
    , Fd_(Setup(fd))
{ }

int TSocketOps::Create(int domain, int type) {
    auto s = socket(domain, type, 0);
    if (s == static_cast<decltype(s)>(-1)) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    return Setup(s);
}

int TSocketOps::Setup(int s) {
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

TSocket::TSocket(TAddress&& addr, TPollerBase& poller, int type)
    : TSocketBase(poller, addr.Domain(), type)
    , Addr_(std::move(addr))
{ }

TSocket::TSocket(const TAddress& addr, int fd, TPollerBase& poller)
    : TSocketBase(fd, poller)
    , Addr_(addr)
{ }

TSocket::TSocket(const TAddress& addr, TPollerBase& poller, int type)
    : TSocketBase(poller, addr.Domain(), type)
    , Addr_(addr)
{ }

TSocket::TSocket(TSocket&& other)
{
    *this = std::move(other);
}

TSocket& TSocket::operator=(TSocket&& other) {
    if (this != &other) {
        Close();
        Poller_ = other.Poller_;
        Addr_ = other.Addr_;
        Fd_ = other.Fd_;
        other.Fd_ = -1;
    }
    return *this;
}

void TSocket::Bind() {
    auto [addr, len] = Addr_.RawAddr();
    int optval = 1;
    socklen_t optlen = sizeof(optval);
    if (setsockopt(Fd_, SOL_SOCKET, SO_REUSEADDR, (char*) &optval, optlen) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
    if (bind(Fd_, addr, len) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }
}

void TSocket::Listen(int backlog) {
    if (listen(Fd_, backlog) < 0) {
        throw std::system_error(errno, std::generic_category(), "listen");
    }
}

const TAddress& TSocket::Addr() const {
    return Addr_;
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
