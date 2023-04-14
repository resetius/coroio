#pragma once

#include <chrono>
#include <stdexcept>
#include <cstring>
#include <coroutine>
#include <tuple>

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

class TTimeout: public std::exception { public: };

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

inline timeval GetTimeval(TTime now, TTime deadline, std::chrono::milliseconds min_duration = std::chrono::milliseconds(10000))
{
    if (now > deadline) {
        return {0,0};
    } else {
        auto duration = (deadline - now);
        if (duration > min_duration) {
            duration = min_duration;
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

inline int GetMillis(TTime now, TTime deadline, std::chrono::milliseconds min_duration = std::chrono::milliseconds(10000)) {
    auto tv = GetTimeval(now, deadline);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

} // namespace NNet
