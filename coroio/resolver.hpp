#pragma once

#include <exception>
#include <unordered_map>

#include "promises.hpp"
#include "socket.hpp"
#include "corochain.hpp"

namespace NNet {

class TResolvConf {
public:
    TResolvConf(const std::string& fn = "/etc/resolv.conf");
    TResolvConf(std::istream& input);

    std::vector<TAddress> Nameservers;

private:
    void Load(std::istream& input);
};

enum class EDNSType {
    DEFAULT = 0,
    A = 1,
    AAAA = 28,
};

template<typename TPoller>
class TResolver {
public:
    TResolver(TPoller& poller, EDNSType defaultType = EDNSType::A);
    TResolver(const TResolvConf& conf, TPoller& poller, EDNSType defaultType = EDNSType::A);
    TResolver(TAddress dnsAddr, TPoller& poller, EDNSType defaultType = EDNSType::A);
    ~TResolver();

    TValueTask<std::vector<TAddress>> Resolve(const std::string& hostname, EDNSType type = EDNSType::DEFAULT);

private:
    TVoidSuspendedTask SenderTask();
    TVoidSuspendedTask ReceiverTask();
    TVoidSuspendedTask TimeoutsTask();

    void ResumeSender();

    TSocket Socket;
    TPoller& Poller;
    EDNSType DefaultType;

    std::coroutine_handle<> Sender;
    std::coroutine_handle<> SenderSuspended;
    std::coroutine_handle<> Receiver;
    std::coroutine_handle<> Timeouts;

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

class THostPort {
public:
    THostPort(const std::string& hostPort);
    THostPort(const std::string& host, int port);

    template<typename T>
    TValueTask<TAddress> Resolve(TResolver<T>& resolver);

private:
    std::string Host;
    int Port;
};

} // namespace NNet
