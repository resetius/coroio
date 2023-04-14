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


} // namespace NNet
