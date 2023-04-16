#pragma once

#include <chrono>
#include <stdexcept>
#include <cstring>
#include <coroutine>
#include <tuple>
#include <iostream>

#include <time.h>

namespace NNet {

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

template<typename T1, typename T2>
inline std::tuple<T1, T2>
GetDurationPair(TTime now, TTime deadline, std::chrono::milliseconds min_duration)
{
    if (now > deadline) {
        return std::make_tuple(T1(0), T2(0));
    } else {
        auto duration = (deadline - now);
        if (duration > min_duration) {
            duration = min_duration;
        }
        auto part1 = std::chrono::duration_cast<T1>(duration);
        duration -= part1;
        auto part2 = std::chrono::duration_cast<T2>(duration);

        return std::make_tuple(part1,part2);
    }
}

inline timeval GetTimeval(TTime now, TTime deadline, std::chrono::milliseconds min_duration = std::chrono::milliseconds(10000))
{
    auto [p1, p2] = GetDurationPair<std::chrono::seconds, std::chrono::microseconds>(now, deadline, min_duration);
    return {p1.count(), static_cast<int>(p2.count())};
}

inline int GetMillis(TTime now, TTime deadline, std::chrono::milliseconds min_duration = std::chrono::milliseconds(10000)) {
    auto tv = GetTimeval(now, deadline);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

inline timespec GetTimespec(TTime now, TTime deadline, std::chrono::milliseconds min_duration = std::chrono::milliseconds(10000))
{
    auto [p1, p2] = GetDurationPair<std::chrono::seconds, std::chrono::nanoseconds>(now, deadline, min_duration);
    return {p1.count(), p2.count()};
}

} // namespace NNet
