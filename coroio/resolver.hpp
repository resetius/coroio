#pragma once

#include "promises.hpp"
#include "socket.hpp"
#include "corochain.hpp"

namespace NNet {

class TResolver {
public:
    TResolver(TAddress dnsAddr);
    ~TResolver();

    TValueTask<std::vector<TAddress>> Resolve(const std::string& hostname);

private:
    TVoidSuspendedTask SenderTask();
    TVoidSuspendedTask ReceiverTask();

    TSocket Socket;

    std::coroutine_handle<> Sender;
    std::coroutine_handle<> Receiver;
    std::queue<std::string> AddResolveQueue;
};

} // namespace NNet
