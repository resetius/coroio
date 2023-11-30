#pragma once

#include <system_error>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <queue>

#include <cstdint>
#include <cstdio>

#include <assert.h>

#include "poller.hpp"

#include "select.hpp"
#include "poll.hpp"

#ifdef __linux__
#include "epoll.hpp"
#include "uring.hpp"
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include "kqueue.hpp"
#endif

#include "loop.hpp"
#include "promises.hpp"
#include "socket.hpp"
#include "corochain.hpp"
#include "sockutils.hpp"