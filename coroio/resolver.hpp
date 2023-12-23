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
    A = 1,
    AAAA = 28,
};

template<typename TPoller>
class TResolver {
public:
    TResolver(TPoller& poller);
    TResolver(const TResolvConf& conf, TPoller& poller);
    TResolver(TAddress dnsAddr, TPoller& poller);
    ~TResolver();

    TValueTask<std::vector<TAddress>> Resolve(const std::string& hostname, EDNSType type = EDNSType::A);

private:
    TVoidSuspendedTask SenderTask();
    TVoidSuspendedTask ReceiverTask();

    void ResumeSender();

    TSocket Socket;
    TPoller& Poller;

    std::coroutine_handle<> Sender;
    std::coroutine_handle<> SenderSuspended;
    std::coroutine_handle<> Receiver;

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

    struct TResolveResult {
        std::vector<TAddress> Addresses;
        std::exception_ptr Exception;
        int Retries = 0;
    };

    std::unordered_map<TResolveRequest, TResolveResult, TResolveRequestHash> Results;
    std::unordered_map<TResolveRequest, std::vector<std::coroutine_handle<>>, TResolveRequestHash> WaitingAddrs;
    std::unordered_map<uint64_t, TResolveRequest> Inflight;

    uint16_t Xid = 1;
};

} // namespace NNet
