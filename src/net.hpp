#pragma once

#include <unordered_map>
#include <vector>
#include <coroutine>
#include <chrono>
#include <iostream>
#include <queue>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

namespace NNet {

class TSystemError: public std::exception
{
public:
    TSystemError()
        : Errno_(errno)
        , Message_(strerror(Errno_))
    { }

    const char* what() const noexcept {
        return Message_.c_str();
    }

    int Errno() const {
        return Errno_;
    }

private:
    int Errno_;
    std::string Message_;
};

struct TVoidPromise;

struct TSimpleTask : std::coroutine_handle<TVoidPromise>
{
    using promise_type = TVoidPromise;
};

struct TVoidPromise
{
    TSimpleTask get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

using TClock = std::chrono::steady_clock;
using TTime = TClock::time_point;
using THandle = std::coroutine_handle<>;

struct TTimer {
    TTime Deadline;
    int Fd;
    THandle Handle;
    bool operator<(const TTimer& e) const {
        return std::tuple(Deadline, Fd) < std::tuple(e.Deadline, e.Fd);
    }
};

struct TEvent {
    THandle Read;
    THandle Write;
    THandle Timeout;
};

class TSelect {
    std::unordered_map<int,TEvent> Events_;
    std::priority_queue<TTimer> Timers_;
    std::vector<THandle> ReadyHandles_;
    TTime Deadline_ = TTime::max();
    fd_set ReadFds_;
    fd_set WriteFds_;

    timeval Timeval() const {
        auto now = TClock::now();
        if (now>Deadline_) {
            return {0,0};
        } else {
            auto duration = (Deadline_-now);
            if (duration > std::chrono::milliseconds(10000)) {
                duration = std::chrono::milliseconds(10000);
            }
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
            duration -= seconds;
            auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration);
            timeval tv;
            tv.tv_sec = seconds.count();
            tv.tv_usec = microseconds.count();
            return tv;
        }
    }

public:
    TSelect() {
        FD_ZERO(&ReadFds_);
        FD_ZERO(&WriteFds_);
    }

    void AddTimer(int fd, TTime deadline, THandle h) {
        Deadline_ = std::min(Deadline_, deadline);
        Timers_.emplace(deadline, fd, h);
        if (fd >= 0) {
            Events_[fd].Timeout = std::move(h);
        }
    }

    void AddRead(int fd, THandle h) {
        Events_[fd].Read = std::move(h);
    }

    void AddWrite(int fd, THandle h) {
        Events_[fd].Write = std::move(h);
    }

    void RemoveEvent(int fd) {
        Events_.erase(fd);
    }

    void Poll() {
        auto tv = Timeval();
        int maxFd = -1;

        FD_ZERO(&ReadFds_);
        FD_ZERO(&WriteFds_);

        for (auto& [k, ev] : Events_) {
            if (ev.Read) {
                FD_SET(k, &ReadFds_);
            }
            if (ev.Write) {
                FD_SET(k, &WriteFds_);
            }
            maxFd = std::max(maxFd, k);
        }
        if (select(maxFd+1, &ReadFds_, &WriteFds_, nullptr, &tv) < 0) { throw TSystemError(); }
        Deadline_ = TTime::max();
        auto now = TClock::now();

        ReadyHandles_.clear();

        for (int k=0; k <= maxFd; ++k) {
            auto& ev = Events_[k];
            if (FD_ISSET(k, &WriteFds_)) {
                ReadyHandles_.emplace_back(std::move(ev.Write));
            }
            if (FD_ISSET(k, &ReadFds_)) {
                ReadyHandles_.emplace_back(std::move(ev.Read));
            }
        }

        while (!Timers_.empty()&&Timers_.top().Deadline <= now) {
            TTimer timer = std::move(Timers_.top());
            if (timer.Fd >= 0) {
                Events_[timer.Fd].Timeout = {};
            }
            ReadyHandles_.emplace_back(timer.Handle);
            Timers_.pop();
        }
    }

    auto& ReadyHandles() {
        return ReadyHandles_;
    }
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

class TLoop {
    TSelect Poller_;
    bool Running_ = true;

public:
    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        auto now = TClock::now();
        auto next= now+duration;
        struct TAwaitable {
            bool await_ready() {
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) {
                loop->Poller().AddTimer(-1, n, h);
            }

            void await_resume() { }

            TLoop* loop;
            TTime n;
        };
        return TAwaitable{this,next};
    }

    void Loop() {
        while (Running_) {
            Poller_.Poll();
            HandleEvents();
        }
    }

    void Stop() {
        Running_ = false;
    }

    void OneStep() {
        Poller_.Poll();
    }

    void HandleEvents() {
        for (auto& ev : Poller_.ReadyHandles()) {
            ev.resume();
        }
    }

    TSelect& Poller() {
        return Poller_;
    }
};

class TSocket {
public:
    TSocket(TAddress&& addr, TSelect& poller)
        : Poller_(&poller)
        , Addr_(std::move(addr))
        , Fd_(Create())
    { }

    TSocket(const TAddress& addr, int fd, TSelect& poller)
        : Poller_(&poller)
        , Addr_(addr)
        , Fd_(fd)
    {
        Setup(Fd_);
    }

    TSocket(const TAddress& addr, TSelect& poller)
        : Poller_(&poller)
        , Addr_(addr)
        , Fd_(Create())
    { }

    TSocket() = default;

    TSocket(const TSocket& other) = delete;

    TSocket(TSocket&& other)
        : Poller_(other.Poller_)
        , Addr_(other.Addr_)
        , Fd_(other.Fd_)
    {
        other.Fd_ = -1;
    }

    ~TSocket()
    {
        if (Fd_ >= 0) {
            close(Fd_);
            Poller_->RemoveEvent(Fd_);
        }
    }

    TSocket& operator=(TSocket&& other) {
        Poller_ = other.Poller_;
        Addr_ = other.Addr_;
        Fd_ = other.Fd_;
        other.Fd_ = -1;
        return *this;
    }

    auto Connect() {
        // TODO: connection timeout
        struct TAwaitable {
            bool await_ready() {
                int ret = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
                if (ret < 0 && !(errno == EINTR||errno==EAGAIN||errno==EINPROGRESS)) {
                    throw TSystemError();
                }
                return ret >= 0;
            }

            void await_suspend(std::coroutine_handle<> h) {
                select->AddWrite(fd, h);
            }

            void await_resume() { }

            TSelect* select;
            int fd;
            sockaddr_in addr;
        };
        return TAwaitable{Poller_, Fd_, Addr_.Addr()};
    }

    auto ReadSome(char* buf, size_t size) {
        struct TAwaitableRead: public TAwaitable<TAwaitableRead> {
            void run() {
                ret = read(fd, b, s);
                if (ret < 0) { err = errno; }
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
                if (ret < 0) { err = errno; }
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

            TSelect* poller;
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
            return (ready = (ret >= 0 || !SkipError(err)));
        }

        int await_resume() {
            if (!ready) {
                SafeRun();
            }
            return ret;
        }

        bool SkipError(int err) {
            return err == EINTR||err==EAGAIN||err==EINPROGRESS;
        }

        void SafeRun() {
            ((T*)this)->run();
            if (ret < 0 && !SkipError(err)) {
                throw TSystemError();
            }
        }

        TSelect* poller;
        int fd;
        char* b; size_t s;
        int ret, err;
        bool ready;
    };

    int Fd_;
    TSelect* Poller_;
    TAddress Addr_;
};

} /* namespace NNet */
