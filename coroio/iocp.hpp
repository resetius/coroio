#pragma once

#include "base.hpp"
#include "socket.hpp"
#include "poller.hpp"

namespace NNet {

class TIOCp: public TPollerBase {
public:
    using TSocket = NNet::TPollerDrivenSocket<TIOCp>;
    using TFileHandle = NNet::TPollerDrivenFileHandle<TIOCp>;

    TIOCp();
    ~TIOCp();

    void Read(int fd, void* buf, int size, std::coroutine_handle<> handle);
    void Write(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    void Recv(int fd, void* buf, int size, std::coroutine_handle<> handle);
    void Send(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle);
    void Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle);

    void Poll();

private:
    long GetTimeoutMs();

    struct TIO {
        WSAOVERLAPPED overlapped;
        TEvent event;

        TIO() {
            memset(&overlapped, 0, sizeof(overlapped));
        }
    };

    HANDLE Port_;

    std::vector<TIO> ReadOps_;
    std::vector<TIO> WriteOps_;
};

}
