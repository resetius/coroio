#pragma once

#include <unordered_map>
#include <vector>
#include <coroutine>
#include <chrono>
#include <iostream>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

using i64 = int64_t;

namespace NNet {

class TEvent {
public:
    TEvent(i64 id, std::coroutine_handle<> h)
        : Id_(id)
        , H_(h)
    { }

    void Handle() {
        H_.resume();
    }

    i64 Id() const {
        return Id_;
    }

private:
    i64 Id_;
    std::coroutine_handle<> H_;
};

class TTimer {
public:
    using TTime = std::chrono::steady_clock::time_point;

    TTimer(i64 id, TTime end)
        : Id_(id)
        , EndTime_(end)
    { }

    i64 Id() const {
        return Id_;
    }

    void Handle() {
        H_.resume();
    }

    auto Awaitable() {
        struct awaitable {
            std::coroutine_handle<>* H;

            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                *H = h;
            }
            void await_resume() { }
        };

        return awaitable{&H_};
    }

    auto EndTime() const {
        return EndTime_;
    }

private:
    i64 Id_;
    TTime EndTime_;
    std::coroutine_handle<> H_;
};

class TSelect {
    std::unordered_map<i64,TTimer> Timers_;
    std::unordered_map<i64,TEvent> Writes_;
    std::unordered_map<i64,TEvent> Reads_;
    std::vector<TTimer> ReadyTimers_;
    std::vector<TEvent> ReadyEvents_;
    bool Running_ = true;
    fd_set ReadFds_;
    fd_set WriteFds_;
    fd_set ErrorFds_;

public:
    TSelect() {
        FD_ZERO(&WriteFds_);
        FD_ZERO(&ErrorFds_);
    }

    TTimer& Add(TTimer&& timer) {
        auto id = timer.Id();
        return Timers_.emplace(id, std::move(timer)).first->second;
    }

    void AddWrite(TEvent&& event) {
        auto id = event.Id();
        Writes_.emplace(id, std::move(event));
    }

    void AddRead(TEvent&& event) {
        auto id = event.Id();
        Reads_.emplace(id, std::move(event));
    }

    bool Ready() {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100;
        i64 max_fd = -1;
        for (auto& [k, _] : Writes_) {
            FD_SET(k, &WriteFds_);
            FD_SET(k, &ErrorFds_);
            max_fd = std::max(max_fd, k);
        }
        // TODO: remove copy paste
        for (auto& [k, _] : Reads_) {
            FD_SET(k, &ReadFds_);
            FD_SET(k, &ErrorFds_);
            max_fd = std::max(max_fd, k);
        }
        select(max_fd+1, &ReadFds_, &WriteFds_, &ErrorFds_, &tv); // TODO: return code
        auto now = std::chrono::steady_clock::now();
        ReadyTimers_.clear();
        for (auto& [k, v]: Timers_) {
            if (now >= v.EndTime()) {
                ReadyTimers_.emplace_back(std::move(v));
            }
        }
        for (auto& v : ReadyTimers_) {
            Timers_.erase(v.Id());
        };

        ReadyEvents_.clear();
        for (auto& [k, v]: Writes_) {
            if (FD_ISSET(k, &ErrorFds_)) {
                int error;
                socklen_t len;
                getsockopt(k, SOL_SOCKET, SO_ERROR, &error, &len);
                std::cerr << "Error " << strerror(error) << "\n";
                // TODO: indicate error
                ReadyEvents_.emplace_back(std::move(v));
            } else if (FD_ISSET(k, &WriteFds_)) {
                ReadyEvents_.emplace_back(std::move(v));
            }
        }
        for (auto& [k, v]: Reads_) {
            if (FD_ISSET(k, &ErrorFds_)) {
                int error;
                socklen_t len;
                getsockopt(k, SOL_SOCKET, SO_ERROR, &error, &len);
                std::cerr << "Error " << strerror(error) << "\n";
                // TODO: indicate error
                ReadyEvents_.emplace_back(std::move(v));
            } else if (FD_ISSET(k, &ReadFds_)) {
                ReadyEvents_.emplace_back(std::move(v));
            }
        }
        // TODO: remove copy-paste
        for (auto& v : ReadyEvents_) {
            Writes_.erase(v.Id());
            Reads_.erase(v.Id());
        }
        return Running_;
    }

    auto& ReadyTimers() {
        return ReadyTimers_;
    }

    auto& ReadyEvents() {
        return ReadyEvents_;
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

    TAddress() { /*TODO:remove*/ }

    auto Addr() const { return Addr_; }

private:
    struct sockaddr_in Addr_;
};

class TLoop {
    TSelect Select_;
    i64 TimerId_ = 0;

public:
    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        auto now = std::chrono::steady_clock::now();
        auto next= now+duration;
        TTimer t(TimerId_++, next);
        return Select_.Add(std::move(t)).Awaitable();
    }

    void Loop() {
        std::vector<TEvent> events;

        while (Select_.Ready()) {
            for (auto& ev : Select_.ReadyTimers()) {
                ev.Handle();
            }
            for (auto& ev : Select_.ReadyEvents()) {
                ev.Handle();
            }
        }
    }

    TSelect& Select() {
        return Select_;
    }
};

class TSocket {
public:
    TSocket(TAddress&& addr, TLoop* loop)
        : Loop_(loop)
        , Addr_(std::move(addr))
        , Fd_(Create())
    { }

    TSocket(const TAddress& addr, int fd, TLoop* loop)
        : Loop_(loop)
        , Addr_(addr)
        , Fd_(fd)
    {
        Setup(Fd_);
    }

    TSocket(const TAddress& addr, TLoop* loop)
        : Loop_(loop)
        , Addr_(addr)
        , Fd_(Create())
    { }

    TSocket(TLoop* loop)
        : Loop_(loop)
        , Addr_{}
        , Fd_(Create())
    {

    }

    ~TSocket()
    {
        close(Fd_);
    }

    auto Connect() {
        auto addr = Addr_.Addr();
        int ret = connect(Fd_, (struct sockaddr*) &addr, sizeof(addr));
        int err = 0;
        if (ret < 0) {
            err = errno;
        }
        // TODO: connection timeout
        struct awaitable {
            bool await_ready() {
                return (ret >= 0 || !(err == EINTR||err==EAGAIN||err==EINPROGRESS));
            }

            void await_suspend(std::coroutine_handle<> h) {
                select.AddWrite(TEvent(fd, h));
            }

            void await_resume() {}

            TSelect& select;
            int ret, err, fd;
        };
        return awaitable{Loop_->Select(), ret,err,Fd_};
    }

    auto Read(char* buf, size_t size) {
        int ret = read(Fd_, buf, size);
        int err = 0;
        if (ret < 0) {
            err = errno;
        }
        // std::cerr << err << " " << strerror(err) << "\n";

        struct awaitable {
            bool await_ready() {
                (ready = (ret >= 0 || !(err == EINTR||err==EAGAIN||err==EINPROGRESS)));
                std::cerr << "R: " << ready << "\n";
                return ready;
            }

            void await_suspend(std::coroutine_handle<> h) {
                std::cerr << "Read Suspended\n";
            }

            int await_resume() {
                if (ready) {
                    return ret;
                } else {
                    return read(fd, b, s);
                }
            }

            TLoop* loop;
            int fd;
            char* b; size_t s;
            int ret, err;
            bool ready;
        };
        return awaitable{Loop_,Fd_,buf,size,ret,err,false};
    }

    auto Write(char* buf, size_t size) {
        int ret = write(Fd_, buf, size);
        int err = 0;
        if (ret < 0) {
            err = errno;
        }

        std::cerr << err << " " << strerror(err) << "\n";

        struct awaitable {
            bool await_ready() {
                return (ready=(ret >= 0 || !(err == EINTR||err==EINTR||err==EINPROGRESS)));
            }

            void await_suspend(std::coroutine_handle<> h) {
                select.AddWrite(TEvent(fd,h));
            }

            int await_resume() {
                if (ready) {
                    return ret;
                } else {
                    ret = write(fd, b, s);
                    std::cerr << errno << " " << strerror(errno) << "\n";
                    return ret;
                }
            }

            TSelect& select;
            int fd, ret, err;
            char* b;
            size_t s;
            bool ready=false;
        };
        return awaitable{Loop_->Select(),Fd_,ret,err,buf,size};
    }

    auto Accept() {
        struct awaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddRead(TEvent(fd, h));
            }
            TSocket await_resume() {
                sockaddr_in clientaddr;
                socklen_t len = sizeof(clientaddr);
                // TODO: return code
                int clientfd = accept(fd, (sockaddr*)&clientaddr, &len);

                return TSocket{clientaddr, clientfd, loop};
            }

            TLoop* loop;
            int fd;
        };

        return awaitable{Loop_, Fd_};
    }

    void Bind() {
        // TODO: return code
        auto addr = Addr_.Addr();
        bind(Fd_, (struct sockaddr*)&addr, sizeof(addr));
    }

    void Listen() {
        // TODO: return code
        // TODO: backlog
        listen(Fd_, 128);
    }

private:
    int Create() {
        /* TODO: return code */
        auto s = socket(PF_INET, SOCK_STREAM, 0);
        Setup(s);
        return s;
    }

    void Setup(int s) {
        int value;
        value = 1;
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(int));
        value = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int));
        auto flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }

    int Fd_;
    TLoop* Loop_;
    TAddress Addr_;
};

} /* namespace NNet */
