#pragma once

#include <unordered_map>

#include "promises.hpp"
#include "socket.hpp"
#include "corochain.hpp"

namespace NNet {

template<typename TPoller>
class TResolver {
public:
    TResolver(TAddress dnsAddr, TPoller& poller);
    ~TResolver();

    TValueTask<std::vector<TAddress>> Resolve(const std::string& hostname);

private:
    TVoidSuspendedTask SenderTask();
    TVoidSuspendedTask ReceiverTask();
    void CreatePacket(const std::string& name, char* buf, int* size);

    TSocket Socket;
    TPoller& Poller;

    std::coroutine_handle<> Sender;
    std::coroutine_handle<> SenderSuspended;
    std::coroutine_handle<> Receiver;
    std::queue<std::string> AddResolveQueue;

    std::unordered_map<std::string, std::vector<TAddress>> Addresses;
    std::unordered_map<std::string, std::vector<std::coroutine_handle<>>> WaitingAddrs;

    uint16_t Xid = 0;
};

} // namespace NNet
