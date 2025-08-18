#pragma once

#include <exception>
#include <unordered_map>

#include "promises.hpp"
#include "socket.hpp"
#include "corochain.hpp"

namespace NNet {

/**
 * @class TResolvConf
 * @brief Reads and stores DNS configuration from a file or an input stream.
 *
 * This class loads a list of nameservers from a DNS configuration file (by default,
 * `/etc/resolv.conf`) or from any other input stream. The nameservers are stored
 * as a vector of @c TAddress objects.
 */
class TResolvConf {
public:
    /**
     * @brief Constructs a TResolvConf and loads DNS configuration from a file.
     *
     * @param fn Path to the DNS configuration file. Default is "/etc/resolv.conf".
     */
    TResolvConf(const std::string& fn = "/etc/resolv.conf");
    /**
     * @brief Constructs a TResolvConf from an input stream.
     *
     * This constructor allows loading the DNS configuration from any standard
     * input stream.
     *
     * @param input Reference to an input stream containing DNS configuration data.
     */
    TResolvConf(std::istream& input);
    /**
     * @brief A vector of nameserver addresses.
     *
     * These addresses are extracted from the configuration source.
     */
    std::vector<TAddress> Nameservers;

private:
    void Load(std::istream& input);
};

/**
 * @enum EDNSType
 * @brief Defines the supported DNS record types for resolution requests.
 */
enum class EDNSType {
    DEFAULT = 0, /**< Use default resolution type; typically this will be the same as A type. */
    A = 1, /**< IPv4 address record. */
    AAAA = 28, /**< IPv6 address record. */
};

struct TTypelessSocket {
    TTypelessSocket() = default;

    template<typename TSocket>
    TTypelessSocket(TSocket&& sock) {
        auto socket = std::make_shared<TSocket>(std::move(sock));
        Connector = [socket](const TAddress& addr, TTime deadline) -> TFuture<void> {
            co_await socket->Connect(addr, deadline);
        };
        Reader = [socket](void* buf, size_t size) -> TFuture<ssize_t> {
            co_return co_await socket->ReadSome(buf, size);
        };
        Writer = [socket](const void* buf, size_t size) -> TFuture<ssize_t> {
            co_return co_await socket->WriteSome(buf, size);
        };
    }

    TFuture<void> Connect(const TAddress& addr, TTime deadline = TTime::max()) {
        co_await Connector(addr, deadline);
    }

    TFuture<ssize_t> ReadSome(void* buf, size_t size) {
        co_return co_await Reader(buf, size);
    }

    TFuture<ssize_t> WriteSome(const void* buf, size_t size) {
        co_return co_await Writer(buf, size);
    }

private:
    std::function<TFuture<void>(const TAddress& addr, TTime deadline)> Connector;
    std::function<TFuture<ssize_t>(void* buf, size_t size)> Reader;
    std::function<TFuture<ssize_t>(const void* buf, size_t size)> Writer;
};

/**
 * @class TResolver
 * @brief Resolves hostnames into IP addresses using a custom poller.
 *
 * The TResolver class provides DNS resolution functionality by sending DNS
 * queries to a nameserver. It supports different DNS record types as specified by
 * EDNSType. Internally, it uses a polling mechanism (provided by the template parameter)
 * for asynchronous operations.
 *
 * @tparam TPoller The type of the poller used to manage asynchronous operations.
 *
 * ### Example Usage
 * @code{.cpp}
 * // Assume TSelect is a poller type with methods Poll() and WakeupReadyHandles()
 * TSelect poller;
 *
 * // Create a resolver using the default nameserver specified in /etc/resolv.conf
 * TResolver<TSelect> resolver(poller);
 *
 * // Asynchronously resolve a hostname (IPv4 resolution by default)
 * TFuture<std::vector<TAddress>> futureAddresses = resolver.Resolve("example.com");
 * // Later (in an asynchronous context) use:
 * // auto addresses = co_await futureAddresses;
 * @endcode
 */
class TResolver {
public:
    /**
     * @brief Constructs a TResolver using the default resolv configuration.
     *
     * The resolver uses DNS nameservers from the default configuration file or
     * as determined by the system.
     *
     * @param poller Reference to a poller used for asynchronous operations.
     * @param defaultType The default DNS record type to use if none is specified in a request.
     */
    template<typename TPoller>
    TResolver(TPoller& poller, EDNSType defaultType = EDNSType::A);
    /**
     * @brief Constructs a TResolver with a specified DNS configuration.
     *
     * This constructor initializes the resolver with nameservers from a given
     * TResolvConf object.
     *
     * @param conf A TResolvConf object containing DNS configuration.
     * @param poller Reference to a poller for asynchronous operations.
     * @param defaultType The default DNS record type to use for resolution.
     */
    template<typename TPoller>
    TResolver(const TResolvConf& conf, TPoller& poller, EDNSType defaultType = EDNSType::A);
    /**
     * @brief Constructs a TResolver using a specific DNS address.
     *
     * This variant allows specifying a single DNS nameserver for resolution.
     *
     * @param dnsAddr The address of the DNS server.
     * @param poller Reference to a poller used for asynchronous operations.
     * @param defaultType The default DNS record type for resolution.
     */
    template<typename TPoller>
    TResolver(TAddress dnsAddr, TPoller& poller, EDNSType defaultType = EDNSType::A);
    ~TResolver();

    /**
     * @brief Resolves a hostname to a list of addresses.
     *
     * Sends a DNS query for the specified hostname. If the request type is set
     * to EDNSType::DEFAULT, the default DNS record type (specified in the
     * constructor) is used.
     *
     * @param hostname The name to be resolved.
     * @param type The DNS record type to query. If EDNSType::DEFAULT is specified,
     *             the default type provided to the constructor is used.
     *
     * @return A TFuture containing a vector of TAddress objects with the resolved addresses.
     */
    TFuture<std::vector<TAddress>> Resolve(const std::string& hostname, EDNSType type = EDNSType::DEFAULT);

private:
    TFuture<void> SenderTask();
    TFuture<void> ReceiverTask();
    TFuture<void> TimeoutsTask(std::function<TFuture<void>(TTime until)> timeout);

    void ResumeSender();

    TFuture<void> Sender;
    TFuture<void> Receiver;
    TFuture<void> Timeouts;
    std::coroutine_handle<> SenderSuspended;

    TAddress DnsAddr;
    TTypelessSocket Socket;
    EDNSType DefaultType;

    struct TResolveRequest {
        std::string Name;
        EDNSType Type;

        bool operator==(const TResolveRequest& other) const {
            return Name == other.Name && Type == other.Type;
        }

        size_t hash() const {
            std::size_t result = 0;
            result ^= std::hash<std::string>()(Name) + 0x9e3779b9 + (result << 6) + (result >> 2);
            result ^= std::hash<EDNSType>()(Type) + 0x9e3779b9 + (result << 6) + (result >> 2);
            return result;
        }
    };

    struct TResolveRequestHash
    {
        std::size_t operator()(const TResolveRequest& c) const
        {
            return c.hash();
        }
    };

    std::queue<TResolveRequest> AddResolveQueue;
    std::queue<std::pair<TTime, TResolveRequest>> TimeoutsQueue;

    struct TResolveResult {
        std::vector<TAddress> Addresses = {};
        std::exception_ptr Exception = nullptr;
        int Retries = 0;
    };

    void ResumeWaiters(TResolveResult&& result, const TResolveRequest& req);

    std::unordered_map<TResolveRequest, TResolveResult, TResolveRequestHash> Results;
    std::unordered_map<TResolveRequest, std::vector<std::coroutine_handle<>>, TResolveRequestHash> WaitingAddrs;
    std::unordered_map<uint64_t, TResolveRequest> Inflight;

    uint16_t Xid = 1;
};

template<typename TPoller>
TResolver::TResolver(TPoller& poller, EDNSType defaultType)
    : TResolver(TResolvConf(), poller, defaultType)
{ }

template<typename TPoller>
TResolver::TResolver(const TResolvConf& conf, TPoller& poller, EDNSType defaultType)
    : TResolver(conf.Nameservers[0], poller, defaultType)
{ }

template<typename TPoller>
TResolver::TResolver(TAddress dnsAddr, TPoller& poller, EDNSType defaultType)
    : DnsAddr(std::move(dnsAddr))
    , Socket(typename TPoller::TSocket(poller, DnsAddr.Domain(), SOCK_DGRAM))
    , DefaultType(defaultType)
{
    // Start tasks after fields initialization
    Sender = SenderTask();
    Receiver = ReceiverTask();
    Timeouts = TimeoutsTask([&poller](TTime until) -> TFuture<void> {
        co_await poller.Sleep(until);
    });
}

class THostPort {
public:
    THostPort(const std::string& hostPort);
    THostPort(const std::string& host, int port);

    TFuture<TAddress> Resolve(TResolver& resolver) {
        char buf[16];
        if (inet_pton(AF_INET, Host.c_str(), buf) == 1 || inet_pton(AF_INET6, Host.c_str(), buf)) {
            co_return TAddress{Host, Port};
        }

        auto addresses = co_await resolver.Resolve(Host);
        if (addresses.empty()) {
            throw std::runtime_error("Empty address");
        }

        co_return addresses.front().WithPort(Port);
    }

    const std::string ToString() const {
        return Host + ":" + std::to_string(Port);
    }

    const std::string& GetHost() const {
        return Host;
    }

    int GetPort() const {
        return Port;
    }

private:
    std::string Host;
    int Port;
};

} // namespace NNet
