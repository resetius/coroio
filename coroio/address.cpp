#include "address.hpp"

#include <string.h>
#include <stdexcept>

namespace NNet {

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

} // namespace NNet