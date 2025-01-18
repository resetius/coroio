#pragma once

#include <system_error>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <queue>

#include <cstdint>
#include <cstdio>

#include <assert.h>

#include "init.hpp"
#include "poller.hpp"

#include "select.hpp"
#include "poll.hpp"

#ifdef __linux__

#ifndef HAVE_EPOLL
#define HAVE_EPOLL
#endif

#ifndef HAVE_URING
#define HAVE_URING
#endif

#include "epoll.hpp"
#include "uring.hpp"
#endif

#ifdef _WIN32

#ifndef HAVE_EPOLL
#define HAVE_EPOLL
#endif

#ifndef HAVE_IOCP
#define HAVE_IOCP
#endif

#include "iocp.hpp"
#include "epoll.hpp"
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)

#ifndef HAVE_KQUEUE
#define HAVE_KQUEUE
#endif

#include "kqueue.hpp"
#endif

#include "loop.hpp"
#include "promises.hpp"
#include "socket.hpp"
#include "corochain.hpp"
#include "sockutils.hpp"
#include "ssl.hpp"
#include "resolver.hpp"

#ifdef _WIN32
int pipe(int pipes[2]);
int socketpair(int domain, int type, int protocol, SOCKET socks[2]);
int socketpair(int domain, int type, int protocol, int socks[2]);
#endif

namespace NNet {
#if defined(__APPLE__) || defined(__FreeBSD__)
using TDefaultPoller = TKqueue;
#elif defined(__linux__)
using TDefaultPoller = TEPoll;
#elif defined(_WIN32)
using TDefaultPoller = TIOCp;
#else
using TDefaultPoller = TPoll;
#endif
} // namespace NNet
