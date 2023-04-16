#pragma once

#include <unordered_map>
#include <vector>
#include <iostream>
#include <queue>

#include <cstdint>
#include <cstdio>

#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include "poller.hpp"

namespace NNet {

struct TVoidPromise;

struct TSimpleTask : std::coroutine_handle<TVoidPromise>
{
    using promise_type = TVoidPromise;
};

struct TVoidPromise
{
    TSimpleTask get_return_object() { return { TSimpleTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

struct TTestPromise;

struct TTestTask : std::coroutine_handle<TTestPromise>
{
    using promise_type = TTestPromise;
};

struct TTestPromise
{
    TTestTask get_return_object() { return { TTestTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

class TAddress {
public:
    TAddress(const std::string& addr, int port)
    {
        inet_pton(AF_INET, addr.c_str(), &(Addr_.sin_addr));
        Addr_.sin_port = htons(port);
        Addr_.sin_family = AF_INET;
    }

    TAddress(sockaddr_in addr)
        : Addr_(addr)
    { }

    TAddress() = default;

    auto Addr() const { return Addr_; }

    bool operator == (const TAddress& other) const {
        return memcmp(&Addr_, &other.Addr_, sizeof(Addr_)) == 0;
    }

private:
    struct sockaddr_in Addr_;
};

template<typename TPoller>
class TLoop {
public:
    void Loop() {
        while (Running_) {
            Step();
        }
    }

    void Stop() {
        Running_ = false;
    }

    void Step() {
        Poller_.Poll();
        HandleEvents();
    }

    TPoller& Poller() {
        return Poller_;
    }

private:
    void HandleEvents() {
        for (auto& ev : Poller_.ReadyHandles()) {
            ev.resume();
        }
    }

    TPoller Poller_;
    bool Running_ = true;
};

class TSocket {
public:
    TSocket(TAddress&& addr, TPollerBase& poller)
        : Poller_(&poller)
        , Addr_(std::move(addr))
        , Fd_(Create())
    { }

    TSocket(const TAddress& addr, int fd, TPollerBase& poller)
        : Poller_(&poller)
        , Addr_(addr)
        , Fd_(fd)
    {
        Setup(Fd_);
    }

    TSocket(const TAddress& addr, TPollerBase& poller)
        : Poller_(&poller)
        , Addr_(addr)
        , Fd_(Create())
    { }

    TSocket(TSocket&& other)
    {
        *this = std::move(other);
    }

    ~TSocket()
    {
        if (Fd_ >= 0) {
            close(Fd_);
            Poller_->RemoveEvent(Fd_);
        }
    }

    TSocket() = default;
    TSocket(const TSocket& other) = delete;
    TSocket& operator=(TSocket& other) const = delete;

    TSocket& operator=(TSocket&& other) {
        if (this != &other) {
            Poller_ = other.Poller_;
            Addr_ = other.Addr_;
            Fd_ = other.Fd_;
            other.Fd_ = -1;
        }
        return *this;
    }

    auto Connect(TTime deadline = TTime::max()) {
        struct TAwaitable {
            bool await_ready() {
                int ret = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
                if (ret < 0 && !(errno == EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
                    throw TSystemError();
                }
                return ret >= 0;
            }

            void await_suspend(std::coroutine_handle<> h) {
                poller->AddWrite(fd, h);
                if (deadline != TTime::max()) {
                    poller->AddTimer(fd, deadline, h);
                }
            }

            void await_resume() {
                if (deadline != TTime::max() && poller->RemoveTimer(fd)) {
                    throw TTimeout();
                }
            }

            TPollerBase* poller;
            int fd;
            sockaddr_in addr;
            TTime deadline;
        };
        return TAwaitable{Poller_, Fd_, Addr_.Addr(), deadline};
    }

    auto ReadSome(char* buf, size_t size) {
        struct TAwaitableRead: public TAwaitable<TAwaitableRead> {
            void run() {
                ret = read(fd, b, s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                poller->AddRead(fd, h);
            }
        };
        return TAwaitableRead{Poller_,Fd_,buf,size};
    }

    // force read on next loop iteration
    auto ReadSomeYield(char* buf, size_t size) {
        struct TAwaitableRead: public TAwaitable<TAwaitableRead> {
            bool await_ready() {
                return (ready = false);
            }

            void run() {
                ret = read(fd, b, s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                poller->AddRead(fd, h);
            }
        };
        return TAwaitableRead{Poller_,Fd_,buf,size};
    }

    auto WriteSome(char* buf, size_t size) {
        struct TAwaitableWrite: public TAwaitable<TAwaitableWrite> {
            void run() {
                ret = write(fd, b, s);
            }

            void await_suspend(std::coroutine_handle<> h) {
                poller->AddWrite(fd, h);
            }
        };
        return TAwaitableWrite{Poller_,Fd_,buf,size};
    }

    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                poller->AddRead(fd, h);
            }
            TSocket await_resume() {
                sockaddr_in clientaddr;
                socklen_t len = sizeof(clientaddr);

                int clientfd = accept(fd, (sockaddr*)&clientaddr, &len);
                if (clientfd < 0) { throw TSystemError(); }

                return TSocket{clientaddr, clientfd, *poller};
            }

            TPollerBase* poller;
            int fd;
        };

        return TAwaitable{Poller_, Fd_};
    }

    void Bind() {
        auto addr = Addr_.Addr();
        if (bind(Fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            throw TSystemError();
        }
    }

    void Listen(int backlog = 128) {
        if (listen(Fd_, backlog) < 0) { throw TSystemError(); }
    }

    const TAddress& Addr() const {
        return Addr_;
    }

    int Fd() const {
        return Fd_;
    }

private:
    int Create() {
        auto s = socket(PF_INET, SOCK_STREAM, 0);
        if (s < 0) { throw TSystemError(); }
        Setup(s);
        return s;
    }

    void Setup(int s) {
        struct stat statbuf;
        fstat(s, &statbuf);
        if (S_ISSOCK(statbuf.st_mode)) {
            int value;
            value = 1;
            if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(int)) < 0) { throw TSystemError(); }
            value = 1;
            if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int)) < 0) { throw TSystemError(); }
        }
        auto flags = fcntl(s, F_GETFL, 0);
        if (flags < 0) { throw TSystemError(); }
        if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) { throw TSystemError(); }
    }

    template<typename T>
    struct TAwaitable {
        bool await_ready() {
            SafeRun();
            return (ready = (ret >= 0));
        }

        int await_resume() {
            if (!ready) {
                SafeRun();
            }
            return ret;
        }

        void SafeRun() {
            ((T*)this)->run();
            if (ret < 0 && !(errno==EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
                throw TSystemError();
            }
        }

        TPollerBase* poller;
        int fd;
        char* b; size_t s;
        int ret;
        bool ready;
    };

    int Fd_ = -1;
    TPollerBase* Poller_ = nullptr;
    TAddress Addr_;
};

} /* namespace NNet */
