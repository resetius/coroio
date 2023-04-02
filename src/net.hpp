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

class TEvent {
public:
    enum EFlag {
        ENONE  = 0,
	EREAD  = 1,
	EWRITE = 2
    };

    using TTime = std::chrono::steady_clock::time_point;

    TEvent(int fd, std::coroutine_handle<> h)
        : Fd_(fd)
        , Deadline_(TTime::max())
        , H_(h)
    { }

    TEvent(TTime deadline, std::coroutine_handle<> h)
        : Fd_(-1)
        , Deadline_(deadline)
        , H_(h)
    { }

    void Handle() {
        H_.resume();
    }

    int Fd() const {
        return Fd_;
    }

    auto Deadline() const {
        return Deadline_;
    }

    void AddFlag(int flag) {
        Flags_ |= flag;
    }

    void ClearFlag(int flag) {
        Flags_ &= ~flag;
    }

    int Flags() { return Flags_; }

    bool operator<(const TEvent& e) const {
        if (Deadline_ < e.Deadline_) {
            return true;
        } else if (Deadline_ == e.Deadline_) {
            return Fd_<e.Fd_;
        } else {
            return false;
        }
    }

private:
    int Fd_ = -1;
    int Flags_ = 0;
    TTime Deadline_;
    std::coroutine_handle<> H_;
};

class TSelect {
    std::unordered_map<int,TEvent> Events_;
    std::priority_queue<TEvent> Timers_;
    std::vector<TEvent> ReadyEvents_;
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

    void AddEvent(int fd, std::coroutine_handle<> h, int flags) {
        auto it = Events_.find(fd);
        if (it == Events_.end()) {
            it = Events_.emplace(fd, TEvent{fd, h}).first;
        }
        it->second.AddFlag(flags);
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
            if (ev.Flags() & TEvent::EREAD) {
                FD_SET(k, &ReadFds_);
            } 
            if (ev.Flags() & TEvent::EWRITE) {
                FD_SET(k, &WriteFds_);
	    }
            maxFd = std::max(maxFd, k);
        }
        if (select(maxFd+1, &ReadFds_, &WriteFds_, nullptr, &tv) < 0) { throw TSystemError(); }
        Deadline_ = TEvent::TTime::max();
        auto now = std::chrono::steady_clock::now();

        ReadyEvents_.clear();

        for (int k=0; k <= maxFd; ++k) {
            int flags = 0;
            if (FD_ISSET(k, &WriteFds_)) {
                flags |= TEvent::EWRITE;
            }
            if (FD_ISSET(k, &ReadFds_)) {
                flags |= TEvent::EREAD;
            }
            if (flags) {
                auto it = Events_.find(k);
                if (it != Events_.end()) {
                    it->second.ClearFlag(flags);
                    ReadyEvents_.emplace_back(it->second);
                }
            }
        }

        while (!Timers_.empty()&&Timers_.top().Deadline() <= now) {
            ReadyEvents_.emplace_back(std::move(Timers_.top()));
            Timers_.pop();
        }
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
    bool Running_ = true;

public:
    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        auto now = std::chrono::steady_clock::now();
        auto next= now+duration;
        struct TAwaitable {
            bool await_ready() {
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddTimer(TEvent{n,h});
            }

            void await_resume() { }

            TLoop* loop;
            TEvent::TTime n;
        };
        return TAwaitable{this,next};
    }

    void Loop() {
        while (Running_) {
            Select_.Poll();
            HandleEvents();
        }
    }

    void Stop() {
        Running_ = false;
    }

    void OneStep() {
        Select_.Poll();
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
            Loop_->Select().RemoveEvent(Fd_);
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
        struct TAwaitable {
            bool await_ready() {
                return (ret >= 0 || !(err == EINTR||err==EAGAIN||err==EINPROGRESS));
            }

            void await_suspend(std::coroutine_handle<> h) {
                select.AddEvent(fd, h, TEvent::EWRITE);
            }

            void await_resume() { }

            TSelect& select;
            int ret, err, fd;
        };
        return TAwaitable{Loop_->Select(), ret,err,Fd_};
    }

    auto ReadSome(char* buf, size_t size) {
        struct TAwaitableRead: public TAwaitable<TAwaitableRead> {
            void run() {
                ret = read(fd, b, s);
                if (ret < 0) { err = errno; }
            }

            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddEvent(fd, h, TEvent::EREAD);
            }
        };
        return TAwaitableRead{Loop_,Fd_,buf,size};
    }

    auto WriteSome(char* buf, size_t size) {
        struct TAwaitableWrite: public TAwaitable<TAwaitableWrite> {
            void run() {
                ret = write(fd, b, s);
                if (ret < 0) { err = errno; }
            }

            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddEvent(fd, h, TEvent::EWRITE);
            }
        };
        return TAwaitableWrite{Loop_,Fd_,buf,size};
    }

    auto Accept() {
        struct TAwaitable {
            bool await_ready() const { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                loop->Select().AddEvent(fd, h, TEvent::EREAD);
            }
            TSocket await_resume() {
                sockaddr_in clientaddr;
                socklen_t len = sizeof(clientaddr);

                int clientfd = accept(fd, (sockaddr*)&clientaddr, &len);
                if (clientfd < 0) { throw TSystemError(); }

                return TSocket{clientaddr, clientfd, loop};
            }

            TLoop* loop;
            int fd;
        };

        return TAwaitable{Loop_, Fd_};
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
