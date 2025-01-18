#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <string>
#include <variant>

namespace NNet {

class TAddress {
public:
    TAddress(const std::string& addr, int port);
    TAddress(sockaddr_in addr);
    TAddress(sockaddr_in6 addr);
    TAddress(sockaddr* addr, socklen_t len);
    TAddress() = default;

    const std::variant<sockaddr_in, sockaddr_in6>& Addr() const;
    std::pair<const sockaddr*, int> RawAddr() const;
    bool operator == (const TAddress& other) const;
    int Domain() const;
    TAddress WithPort(int port) const;

    std::string ToString() const;

private:
    std::variant<sockaddr_in, sockaddr_in6> Addr_ = {};
};

} // namespace NNet