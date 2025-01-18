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

/**
 * @class TAddress
 * @brief A class representing an IPv4 or IPv6 address (with port).
 *
 * This class acts as a generic container for both IPv4 (sockaddr_in)
 * and IPv6 (sockaddr_in6) addresses, providing a uniform interface
 * for common operations (e.g., string conversion, retrieving raw data).
 */
class TAddress {
public:
    /**
     * @brief Constructs an address from a string representation and port.
     *
     * @param addr A string containing an IPv4 or IPv6 address in standard notation
     *             (e.g., "127.0.0.1" or "::1").
     * @param port The port number associated with the address.
     *
     * @exception std::runtime_error Thrown if the provided string cannot be
     *                               resolved to a valid IP address.
     */
    TAddress(const std::string& addr, int port);
    /**
     * @brief Constructs an address from an IPv4 sockaddr_in structure.
     *
     * @param addr A valid sockaddr_in (IPv4) structure containing an IP and port.
     */
    TAddress(sockaddr_in addr);
    /**
     * @brief Constructs an address from an IPv6 sockaddr_in6 structure.
     *
     * @param addr A valid sockaddr_in6 (IPv6) structure containing an IP and port.
     */
    TAddress(sockaddr_in6 addr);
    /**
     * @brief Constructs an address from a generic sockaddr pointer.
     *
     * The provided pointer is assumed to point to either a sockaddr_in
     * (for IPv4) or a sockaddr_in6 (for IPv6). The @p len parameter should
     * match the actual size of the corresponding address structure.
     *
     * @param addr Pointer to a valid sockaddr struct (IPv4 or IPv6).
     * @param len  The size of the given @p addr structure.
     */
    TAddress(sockaddr* addr, socklen_t len);
    /**
     * @brief Default constructor. Creates an empty address.
     *
     * The address variant is not set to IPv4 or IPv6 in this case.
     * Attempting to use domain-specific functionality on an empty address
     * can lead to undefined behavior, so ensure you assign or construct
     * it properly before use.
     */
    TAddress() = default;
    /**
     * @brief Retrieves the internal variant storing the address.
     *
     * The variant can hold either an IPv4 (sockaddr_in) or
     * an IPv6 (sockaddr_in6) structure.
     *
     * @return A constant reference to the variant holding the address.
     */
    const std::variant<sockaddr_in, sockaddr_in6>& Addr() const;
    /**
     * @brief Returns a pointer to the raw sockaddr structure and its size in bytes.
     *
     * Useful when calling low-level socket functions (e.g., connect, bind, sendto) that
     * require a pointer to a sockaddr. The size indicates whether it is IPv4 or IPv6.
     *
     * @return A std::pair containing:
     *         - `first`: A pointer to the sockaddr (casted to sockaddr*).
     *         - `second`: The size of the sockaddr structure.
     */
    std::pair<const sockaddr*, int> RawAddr() const;
    /**
     * @brief Equality operator for TAddress.
     *
     * Two addresses are considered equal if they share the same protocol family
     * (IPv4 vs. IPv6) and identical IP and port values.
     *
     * @param other Another TAddress instance to compare with.
     * @return True if both addresses are equal; otherwise false.
     */
    bool operator == (const TAddress& other) const;
    /**
     * @brief Gets the domain (address family) of the stored address.
     *
     * Returns AF_INET for IPv4 or AF_INET6 for IPv6.
     *
     * @return The address family (e.g., AF_INET or AF_INET6).
     */
    int Domain() const;
    /**
     * @brief Returns a new TAddress with the same IP but a different port.
     *
     * This is a convenience method for changing only the port, leaving
     * the IP portion unmodified.
     *
     * @param port The new port to set.
     * @return A new TAddress instance with the same IP but updated port.
     */
    TAddress WithPort(int port) const;
    /**
     * @brief Converts the address to a human-readable string format.
     *
     * Examples:
     * - IPv4: "127.0.0.1:8080"
     * - IPv6: "[::1]:9090"
     *
     * @return A string containing the IP address and port.
     */
    std::string ToString() const;

private:
    std::variant<sockaddr_in, sockaddr_in6> Addr_ = {};
};

} // namespace NNet