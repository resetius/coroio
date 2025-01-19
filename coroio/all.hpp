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
#include "address.hpp"
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

/**
 * @mainpage Asynchronous I/O & Networking Library
 *
 * @section intro_sec Introduction
 *
 * This library provides advanced asynchronous I/O and networking capabilities using
 * coroutine-based APIs and poller-driven execution. It supports high-performance operations
 * for both network sockets and file handles.
 *
 * @section features_sec Key Features
 *
 * The library’s core functionality is exposed through a few high-level classes:
 * - @ref TSocket and @ref TPollerDrivenSocket for asynchronous networking.
 * - @ref TFileHandle and @ref TPollerDrivenFileHandle for asynchronous file I/O.
 * - @ref TLineReader for efficient, line-based input.
 * - @ref TByteReader and @ref TByteWriter for byte-level I/O.
 * - @ref TResolver and @ref TResolvConf for DNS resolution.
 *
 * In addition to these, the library supports multiple polling mechanisms for asynchronous operations:
 *
 * - @ref TSelect - Poller using the select() system call.
 * - @ref TPoll - Poller using the poll() system call.
 * - @ref TEPoll - Linux-specific poller using epoll.
 * - @ref TKqueue - Poller for macOS/FreeBSD using kqueue.
 * - @ref TIOCp - Poller for Windows using IOCP.
 * - @ref TUring - Linux-specific poller using io_uring.
 *
 * @section example_sec Example: Echo Client
 *
 * The following example demonstrates a simple echo client. The client reads lines from standard input,
 * sends them to an echo server, and then prints the response.
 *
 * @code{.cpp}
 * template<bool debug, typename TPoller>
 * TFuture<void> client(TPoller& poller, TAddress addr)
 * {
 *     static constexpr int maxLineSize = 4096;
 *     using TSocket = typename TPoller::TSocket;
 *     using TFileHandle = typename TPoller::TFileHandle;
 *     std::vector<char> in(maxLineSize);
 *
 *     try {
 *         TFileHandle input{0, poller}; // stdin
 *         TSocket socket{poller, addr.Domain()};
 *         TLineReader lineReader(input, maxLineSize);
 *         TByteWriter byteWriter(socket);
 *         TByteReader byteReader(socket);
 *
 *         co_await socket.Connect(addr, TClock::now() + std::chrono::milliseconds(1000));
 *         while (auto line = co_await lineReader.Read()) {
 *             co_await byteWriter.Write(line);
 *             co_await byteReader.Read(in.data(), line.Size());
 *             if constexpr (debug) {
 *                 std::cout << "Received: " << std::string_view(in.data(), line.Size()) << "\n";
 *             }
 *         }
 *     } catch (const std::exception& ex) {
 *         std::cout << "Exception: " << ex.what() << "\n";
 *     }
 *
 *     co_return;
 * }
 * @endcode
 *
 * @section further_sec Further Information
 *
 * For more details on each component, please refer to the following classes:
 * - @ref TSocket and @ref TPollerDrivenSocket
 * - @ref TFileHandle and @ref TPollerDrivenFileHandle
 * - @ref TLineReader, @ref TByteReader, and @ref TByteWriter
 * - @ref TResolver and @ref TResolvConf
 *
 * For the available pollers, see:
 * - @ref TSelect
 * - @ref TPoll
 * - @ref TEPoll
 * - @ref TKqueue
 * - @ref TIOCp
 * - @ref TUring
 */

} // namespace NNet
