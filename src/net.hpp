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
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

using i64 = int64_t;

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

class TEvent {
public:
    using TTime = std::chrono::steady_clock::time_point;

    TEvent(i64 id, std::coroutine_handle<> h)
        : Id_(id)
        , H_(h)
    { }

    TEvent(TTime deadline, i64 id, std::coroutine_handle<> h)
        : Id_(id)
        , Deadline_(deadline)
        , H_(h)
    { }

    void Handle() {
        H_.resume();
    }

    i64 Id() const {
        return Id_;
    }

    auto Deadline() const {
        return Deadline_;
    }

    bool operator<(const TEvent& e) const {
        if (Deadline_ < e.Deadline_) {
            return true;
        } else if (Deadline_ == e.Deadline_) {
            return Id_<e.Id_;
        } else {
            return false;
        }
    }

private:
    i64 Id_;
    TTime Deadline_;
    std::coroutine_handle<> H_;
};

class TSelect {
    std::unordered_map<i64,TEvent> Writes_;
    std::unordered_map<i64,TEvent> Reads_;
    std::priority_queue<TEvent> Timers_;
    std::vector<TEvent> ReadyEvents_;
    bool Running_ = true;
    TEvent::TTime Deadline_ = TEvent::TTime::max();
    fd_set ReadFds_;
    fd_set WriteFds_;

    timeval Timeval() const {
        auto now = std::chrono::steady_clock::now();
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

    void AddTimer(TEvent&& timer) {
        Deadline_ = std::min(Deadline_, timer.Deadline());
        Timers_.emplace(std::move(timer));
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
        auto tv = Timeval();
        i64 max_fd = -1;

        FD_ZERO(&ReadFds_);
        FD_ZERO(&WriteFds_);

        for (auto& [k, _] : Writes_) {
            FD_SET(k, &WriteFds_);
            max_fd = std::max(max_fd, k);
        }
        // TODO: remove copy paste
        for (auto& [k, _] : Reads_) {
            FD_SET(k, &ReadFds_);
            max_fd = std::max(max_fd, k);
        }
        select(max_fd+1, &ReadFds_, &WriteFds_, nullptr, &tv); // TODO: return code
        Deadline_ = TEvent::TTime::max();
        auto now = std::chrono::steady_clock::now();

        ReadyEvents_.clear();

        for (auto& [k, v]: Writes_) {
            if (FD_ISSET(k, &WriteFds_)) {
                ReadyEvents_.emplace_back(std::move(v));
            }
        }
        for (auto& [k, v]: Reads_) {
            if (FD_ISSET(k, &ReadFds_)) {
                ReadyEvents_.emplace_back(std::move(v));
            }
        }
        // TODO: remove copy-paste
        for (auto& v : ReadyEvents_) {
            Writes_.erase(v.Id());
            Reads_.erase(v.Id());
        }
        // TODO: timer id collision with fd
        while (!Timers_.empty()&&Timers_.top().Deadline() <= now) {
            ReadyEvents_.emplace_back(std::move(Timers_.top()));
            Timers_.pop();
        }

        return Running_;
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

    TAddress() = default;

    auto Addr() const { return Addr_; }

    bool operator == (const TAddress& other) const {
        return memcmp(&Addr_, &other.Addr_, sizeof(Addr_)) == 0;
    }

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
        struct awaitable {
            bool await_ready() {
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddTimer(TEvent{n,id,h});
            }

            void await_resume() { }

            TLoop* loop;
            i64 id;
            TEvent::TTime n;
        };
        return awaitable{this,TimerId_++,next};
    }

    void Loop() {
        while (Select_.Ready()) {
            HandleEvents();
        }
    }

    void OneStep() {
        Select_.Ready();
    }

    void HandleEvents() {
        for (auto& ev : Select_.ReadyEvents()) {
            ev.Handle();
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

    TSocket() = default;

    TSocket(const TSocket& other) = delete;

    TSocket(TSocket&& other)
        : Loop_(other.Loop_)
        , Addr_(other.Addr_)
        , Fd_(other.Fd_)
    {
        other.Fd_ = -1;
    }

    ~TSocket()
    {
        if (Fd_ >= 0) {
            close(Fd_);
        }
    }

    TSocket& operator=(TSocket&& other) {
        Loop_ = other.Loop_;
        Addr_ = other.Addr_;
        Fd_ = other.Fd_;
        other.Fd_ = -1;
        return *this;
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
        struct TAwaitableRead: public TAwaitable<TAwaitableRead> {
            void run() {
                ret = read(fd, b, s);
                if (ret < 0) { err = errno; }
            }

            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddRead(TEvent{fd,h});
            }
        };
        return TAwaitableRead{Loop_,Fd_,buf,size};
    }

    auto Write(char* buf, size_t size) {
        struct TAwaitableWrite: public TAwaitable<TAwaitableWrite> {
            void run() {
                ret = write(fd, b, s);
                if (ret < 0) { err = errno; }
            }

            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddWrite(TEvent(fd,h));
            }
        };
        return TAwaitableWrite{Loop_,Fd_,buf,size};
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

    const TAddress& Addr() const {
        return Addr_;
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

        TLoop* loop;
        int fd;
        char* b; size_t s;
        int ret, err;
        bool ready;
    };

    int Fd_;
    TLoop* Loop_;
    TAddress Addr_;
};

} /* namespace NNet */
