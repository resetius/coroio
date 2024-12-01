#pragma once

#include "base.hpp"
#include "socket.hpp"
#include "poller.hpp"

#include <liburing.h>
#include <assert.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/utsname.h>

#include <system_error>
#include <iostream>
#include <tuple>
#include <vector>
#include <coroutine>
#include <queue>

namespace NNet {

class TUring: public TPollerBase {
public:
    using TSocket = NNet::TPollerDrivenSocket<TUring>;
    using TFileHandle = NNet::TFileHandle;

    TUring(int queueSize = 256);

    ~TUring();

    void Read(int fd, void* buf, int size, std::coroutine_handle<> handle);
    void Write(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle);
    void Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle);
    void Cancel(int fd);
    void Cancel(std::coroutine_handle<> h);

    int Wait(timespec ts = {10,0});

    void Poll() {
        Wait(GetTimeout());
    }

    int Result();

    void Submit();

    // for tests
    std::tuple<int, int, int> Kernel() const;
    const std::string& KernelStr() const;

private:
    io_uring_sqe* GetSqe() {
        io_uring_sqe* r = io_uring_get_sqe(&Ring_);
        if (!r) {
            Submit();
            r = io_uring_get_sqe(&Ring_);
        }
        return r;
    }

    int RingFd_;
    int EpollFd_;
    struct io_uring Ring_;
    std::queue<int> Results_;
    std::vector<char> Buffer_;
    std::tuple<int, int, int> Kernel_;
    std::string KernelStr_;
};

} // namespace NNet
