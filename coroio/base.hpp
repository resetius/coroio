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
    unsigned Id;
    THandle Handle;
    bool operator<(const TTimer& e) const {
        return std::tuple(Deadline, Id, static_cast<bool>(Handle)) > std::tuple(e.Deadline, e.Id, static_cast<bool>(e.Handle));
    }
};

struct THandlePair {
    THandle Read;
    THandle Write;
};

struct TEvent {
    int Fd;
    enum {
        READ = 1,
        WRITE = 2
    };
    int Type;
    THandle Handle;

    bool Match(const TEvent& other) const {
        return Fd == other.Fd && (Type & other.Type);
    }
};

template<typename T1, typename T2>
inline std::tuple<T1, T2>
GetDurationPair(TTime now, TTime deadline, std::chrono::milliseconds maxDuration)
{
    if (now > deadline) {
        return std::make_tuple(T1(0), T2(0));
    } else {
        auto duration = (deadline - now);
        if (duration > maxDuration) {
            duration = maxDuration;
        }
        auto part1 = std::chrono::duration_cast<T1>(duration);
        duration -= part1;
        auto part2 = std::chrono::duration_cast<T2>(duration);

        return std::make_tuple(part1,part2);
    }
}

inline timespec GetTimespec(TTime now, TTime deadline, std::chrono::milliseconds maxDuration)
{
    auto [p1, p2] = GetDurationPair<std::chrono::seconds, std::chrono::nanoseconds>(now, deadline, maxDuration);
    timespec ret;
    ret.tv_sec = p1.count();
    ret.tv_nsec = p2.count();
    return ret;
}

} // namespace NNet
