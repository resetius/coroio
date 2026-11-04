#include <atomic>
#include <barrier>
#include <thread>
#include <chrono>
#include <array>
#include <deque>
#include <list>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>

#include <coroio/all.hpp>
#include <coroio/ws/utils.hpp>
#include <coroio/dns/resolver.hpp>

#include <unordered_set>

#include "testlib.h"

extern "C" {
#include <cmocka.h>
}

#include "server.crt"
#include "server.key"

namespace {

static uint32_t rand_(uint32_t* seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

#ifdef _WIN32
constexpr int ConnRefused = WSAECONNREFUSED;
#else
constexpr int ConnRefused = ECONNREFUSED;
#endif

} // namespace

using namespace NNet;

static constexpr std::chrono::milliseconds maxDiration(10000);

void test_timespec(void**) {
    auto t1 =  std::chrono::seconds(4);
    auto t2 =  std::chrono::seconds(10);
    auto ts = GetTimespec(TTime(t1), TTime(t2), maxDiration);
    assert_int_equal(ts.tv_sec, 6);
    assert_int_equal(ts.tv_nsec, 0);

    auto t3 =  std::chrono::milliseconds(10001);
    ts = GetTimespec(TTime(t1), TTime(t3), maxDiration);
    assert_int_equal(ts.tv_sec, 6);
    assert_int_equal(ts.tv_nsec, 1000*1000);

    auto t4 = std::chrono::minutes(10000);
    ts = GetTimespec(TTime(t1), TTime(t4), maxDiration);
    assert_int_equal(ts.tv_sec, 10);
    assert_int_equal(ts.tv_nsec, 0);
}

void test_addr(void**) {
    int port = GetFreePort();
    TAddress address("127.0.0.1", port);
    auto low = std::get<sockaddr_in>(address.Addr());
    assert_true(low.sin_port == ntohs(port));
    assert_true(low.sin_family == AF_INET);

    unsigned int value = ntohl((127<<24)|(0<<16)|(0<<8)|1);
    assert_true(memcmp(&low.sin_addr, &value, 4) == 0);
}

void test_addr6(void**) {
    int port = GetFreePort();
    TAddress address("::1", port);
    auto low = std::get<sockaddr_in6>(address.Addr());
    assert_true(low.sin6_port == ntohs(port));
    assert_true(low.sin6_family == AF_INET6);
}

void test_bad_addr(void**) {
    int port = GetFreePort();
    int flag = 0;
    try {
        TAddress address("wtf", port);
    } catch (const std::exception& ex) {
        flag = 1;
    }
    assert_int_equal(flag, 1);
}

template<typename TPoller>
void test_listen(void**) {
    int port = GetFreePort();
    TLoop<TPoller> loop;
    TAddress address("127.0.0.1", port);
    TSocket socket(loop.Poller(), address.Domain());
    socket.Bind(address);
    socket.Listen();
}

template<typename TPoller>
void test_accept(void**) {
    int port = GetFreePort();
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    TAddress addr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), addr.Domain());
    TSocket clientSocket{};
    socket.Bind(addr);
    socket.Listen();

    TFuture<void> h1 = [](TPoller& poller, int port) -> TFuture<void>
    {
        try {
            TAddress addr{"127.0.0.1", port};
            TSocket client(poller, addr.Domain());
            co_await client.Connect(addr);
        } catch (const std::exception& ex) {
            std::cerr << "Error on connect: " << ex.what() << std::endl;
        }
        co_return;
    }(loop.Poller(), port);

    TFuture<void> h2 = [](TSocket* socket, TSocket* clientSocket) -> TFuture<void>
    {
        try {
            *clientSocket = std::move(co_await socket->Accept());
        } catch (const std::exception& ex) {
            std::cerr << "Error on accept: " << ex.what() << std::endl;
        }
        co_return;
    }(&socket, &clientSocket);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    in_addr addr1 = std::get<sockaddr_in>(clientSocket.RemoteAddr()->Addr()).sin_addr;
    in_addr addr2 = std::get<sockaddr_in>(socket.LocalAddr()->Addr()).sin_addr;
    assert_memory_equal(&addr1, &addr2, 4);
}

#ifndef _WIN32
template<typename TPoller>
void test_cloexec(void**) {
    int port = GetFreePort();
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    TAddress addr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), addr.Domain());
    TSocket client(loop.Poller(), addr.Domain());
    TSocket accepted{};
    socket.Bind(addr);
    socket.Listen();

    TFuture<void> h1 = [](TSocket* client, TAddress addr) -> TFuture<void>
    {
        try {
            co_await client->Connect(addr);
        } catch (const std::exception& ex) {
            std::cerr << "Error on connect: " << ex.what() << std::endl;
        }
        co_return;
    }(&client, addr);

    TFuture<void> h2 = [](TSocket* socket, TSocket* accepted) -> TFuture<void>
    {
        try {
            *accepted = std::move(co_await socket->Accept());
        } catch (const std::exception& ex) {
            std::cerr << "Error on accept: " << ex.what() << std::endl;
        }
        co_return;
    }(&socket, &accepted);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    for (int fd : {socket.Fd(), client.Fd(), accepted.Fd()}) {
        int flags = fcntl(fd, F_GETFD);
        assert_true(flags >= 0);
        assert_true(flags & FD_CLOEXEC);
    }
}
#endif

template<typename TPoller>
void test_write_after_connect(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    TAddress addr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), addr.Domain());
    socket.Bind(addr);
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[256] = {0};

    TFuture<void> h1 = [](TPoller& poller, char* buf, int size, int port) -> TFuture<void>
    {
        try {
            TAddress addr{"127.0.0.1", port};
            TSocket client(poller, addr.Domain());
            co_await client.Connect(addr);
            co_await client.WriteSome(buf, size);
        } catch (const std::exception& ex) {
            std::cerr << "Error1: " << ex.what() << "\n";
        }
        co_return;
    }(loop.Poller(), send_buf, sizeof(send_buf), port);

    TFuture<void> h2 = [](TSocket* socket, char* buf, int size) -> TFuture<void>
    {
        try {
            TSocket clientSocket = std::move(co_await socket->Accept());
            co_await clientSocket.ReadSome(buf, size);
        } catch (const std::exception& ex) {
            std::cerr << "Error2: " << ex.what() << "\n";
        }
        co_return;
    }(&socket, rcv_buf, sizeof(rcv_buf));

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_true(memcmp(&send_buf, &rcv_buf, sizeof(send_buf))==0);
}

template<typename TPoller>
void test_write_after_accept(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    TAddress addr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), addr.Domain());
    socket.Bind(addr);
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[256] = {0};

    TFuture<void> h1 = [](TPoller& poller, char* buf, int size, int port) -> TFuture<void>
    {
        try {
            TAddress addr{"127.0.0.1", port};
            TSocket client(poller, addr.Domain());
            co_await client.Connect(addr);
            co_await client.ReadSome(buf, size);
        } catch (const std::exception& ex) {
            std::cerr << "Error1: " << ex.what() << "\n";
        }
        co_return;
    }(loop.Poller(), rcv_buf, sizeof(rcv_buf), port);

    TFuture<void> h2 = [](TSocket* socket, char* buf, int size) -> TFuture<void>
    {
        try {
            TSocket clientSocket = std::move(co_await socket->Accept());
            auto s = co_await clientSocket.WriteSome(buf, size);
        } catch (const std::exception& ex) {
            std::cerr << "Error2: " << ex.what() << "\n";
        }
        co_return;
    }(&socket, send_buf, sizeof(send_buf));

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_true(memcmp(&send_buf, &rcv_buf, sizeof(send_buf))==0);
}

template<typename TPoller>
void test_read_write_same_socket(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    TAddress saddr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), saddr.Domain());
    socket.Bind(saddr);
    socket.Listen();
    char buf1[128] = {0};
    char buf2[128] = {0};

    TAddress caddr{"127.0.0.1", port};
    TSocket client(loop.Poller(), caddr.Domain());

    TFuture<void> h1 = [](TSocket& client, TAddress& caddr) -> TFuture<void>
    {
        co_await client.Connect(caddr);
        co_return;
    }(client, caddr);

    TFuture<void> h2 = [](TSocket* socket, char* buf, int size) -> TFuture<void>
    {
        TSocket clientSocket = std::move(co_await socket->Accept());
        char b[128] = "Hello from server";
        co_await clientSocket.WriteSomeYield(b, sizeof(b));
        co_await clientSocket.ReadSomeYield(buf, size);
        co_return;
    }(&socket, buf1, sizeof(buf1));

    while (!h1.done()) {
        loop.Step();
    }

    TFuture<void> h3 = [](TSocket& client) -> TFuture<void>
    {
        char b[128] = "Hello from client";
        co_await client.WriteSomeYield(b, sizeof(b));
        co_return;
    }(client);

    TFuture<void> h4 = [](TSocket& client, char* buf, int size) -> TFuture<void>
    {
        co_await client.ReadSomeYield(buf, size);
        co_return;
    }(client, buf2, sizeof(buf2));

    while (!(h1.done() && h2.done() && h3.done() && h4.done())) {
        loop.Step();
    }

    assert_string_equal(buf1, "Hello from client");
    assert_string_equal(buf2, "Hello from server");
}

template<typename TPoller>
void test_connection_timeout(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    TAddress addr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), addr.Domain());
    socket.Bind(addr);
    // Once the accept queue is full, a new SYN is dropped (Linux, macOS) or
    // refused and retried for seconds (Windows), so the connect stays pending.
    socket.Listen(1);
    std::vector<TSocket> queued;
    bool timeout = false;

    TFuture<void> h = [](TPoller& poller, std::vector<TSocket>& queued, bool& timeout, int port) -> TFuture<void>
    {
        TAddress addr{"127.0.0.1", port};
        while (!timeout && queued.size() < 16) {
            TSocket client(poller, addr.Domain());
            try {
                co_await client.Connect(addr, TClock::now()+std::chrono::milliseconds(100));
                queued.emplace_back(std::move(client));
            } catch (const std::system_error& ex) {
                if (ex.code() == std::errc::timed_out) {
                    timeout = true;
                } else {
                    throw;
                }
            }
        }
        co_return;
    }(loop.Poller(), queued, timeout, port);

    while (!h.done()) {
        loop.Step();
    }

    assert_true(timeout);
}

template<typename TPoller>
void test_connection_timeout_cleanup(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    TAddress addr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), addr.Domain());
    socket.Bind(addr);
    socket.Listen();
    TSocket client(loop.Poller(), addr.Domain());
    bool timeout = false;

    {
        // The deadline has already passed: the timer wins, the connect itself succeeds.
        TFuture<void> h = [](TSocket& client, bool& timeout, int port) -> TFuture<void>
        {
            TAddress addr{"127.0.0.1", port};
            try {
                co_await client.Connect(addr, TClock::now());
            } catch (const std::system_error& ex) {
                if (ex.code() == std::errc::timed_out) {
                    timeout = true;
                } else {
                    throw;
                }
            }
            co_return;
        }(client, timeout, port);

        while (!h.done()) {
            loop.Step();
        }
    }

    // A wait left in the poller would resume the destroyed coroutine here.
    loop.Step();

    assert_true(timeout);
}

template<typename TPoller>
void test_remove_connection_timeout(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    TAddress addr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), addr.Domain());
    socket.Bind(addr);
    socket.Listen();

    bool timeout = false;

    TFuture<void> h = [](TPoller& poller, bool& timeout, int port) -> TFuture<void>
    {
        TAddress addr{"127.0.0.1", port};
        TSocket client(poller, addr.Domain());
        try {
            co_await client.Connect(addr, TClock::now()+std::chrono::milliseconds(10));
            co_await poller.Sleep(std::chrono::milliseconds(100));
        } catch (const std::system_error& ex) {
            if (ex.code() == std::errc::timed_out) {
                timeout = true;
            } else {
                throw;
            }
        }
        co_return;
    }(loop.Poller(), timeout, port);

    while (!h.done()) {
        loop.Step();
    }

    assert_false(timeout);
}

template<typename TPoller>
void test_connection_refused_on_write(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    std::error_code err;
    bool connected = false;

    TFuture<void> h = [](TPoller& poller, std::error_code* err, bool* connected, int port) -> TFuture<void>
    {
        TAddress addr{"127.0.0.1", port};
        TSocket clientSocket(poller, addr.Domain());
        char buffer[] = "test";
        try {
            // The deadline only guards against a hang: Windows retries a refused SYN for about 2 seconds.
            co_await clientSocket.Connect(addr, TClock::now()+std::chrono::seconds(10));
            *connected = true;
            co_await clientSocket.WriteSome(buffer, sizeof(buffer));
        } catch (const std::system_error& ex) {
            *err = ex.code();
        }
        co_return;
    }(loop.Poller(), &err, &connected, port);

    while (!h.done()) {
        loop.Step();
    }

    assert_false(connected);
    assert_int_equal(err.value(), ConnRefused);
}

template<typename TPoller>
void test_connection_refused_on_read(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    std::error_code err;
    bool connected = false;

    TFuture<void> h = [](TPoller& poller, std::error_code* err, bool* connected, int port) -> TFuture<void>
    {
        TAddress addr{"127.0.0.1", port};
        TSocket clientSocket(poller, addr.Domain());
        char buffer[] = "test";
        try {
            // The deadline only guards against a hang: Windows retries a refused SYN for about 2 seconds.
            co_await clientSocket.Connect(addr, TClock::now()+std::chrono::seconds(10));
            *connected = true;
            co_await clientSocket.ReadSome(buffer, sizeof(buffer));
        } catch (const std::system_error& ex) {
            *err = ex.code();
        }
        co_return;
    }(loop.Poller(), &err, &connected, port);

    while (!h.done()) {
        loop.Step();
    }

    assert_false(connected);
    assert_int_equal(err.value(), ConnRefused);
}

template<typename TPoller>
void test_connection_refused(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    std::error_code err;

    TFuture<void> h = [](TPoller& poller, std::error_code* err, int port) -> TFuture<void>
    {
        TAddress addr{"127.0.0.1", port};
        TSocket clientSocket(poller, addr.Domain());
        try {
            co_await clientSocket.Connect(addr);
        } catch (const std::system_error& ex) {
            *err = ex.code();
        }
        co_return;
    }(loop.Poller(), &err, port);

    while (!h.done()) {
        loop.Step();
    }

    assert_int_equal(err.value(), ConnRefused);
}

template<typename TPoller>
void test_timeout(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(100);
    TTime next;
    TFuture<void> h = [](TPollerBase& poller, TTime* next, std::chrono::milliseconds timeout) -> TFuture<void>
    {
        co_await poller.Sleep(timeout);
        *next = std::chrono::steady_clock::now();
        co_return;
    } (loop.Poller(), &next, timeout);

    while (!h.done()) {
        loop.Step();
    }

    assert_true(next >= now + timeout);
}

template<typename TPoller>
void test_timeout2(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    auto now = std::chrono::steady_clock::now();
    auto timeout1 = std::chrono::milliseconds(100);
    auto timeout2 = std::chrono::milliseconds(200);
    int val1, val2, val;
    val1 = val2 = val = 0;
    TFuture<void> h1 = [](TPollerBase& poller, std::chrono::milliseconds timeout, int* val1, int* val) -> TFuture<void>
    {
        co_await poller.Sleep(timeout);
        (*val)++;
        *val1 = *val;
        co_return;
    } (loop.Poller(), timeout1, &val1, &val);

    TFuture<void> h2 = [](TPollerBase& poller, std::chrono::milliseconds timeout, int* val1, int* val) -> TFuture<void>
    {
        co_await poller.Sleep(timeout);
        (*val)++;
        *val1 = *val;
        co_return;
    } (loop.Poller(), timeout2, &val2, &val);

    while (!h1.done() || !h2.done()) {
        loop.Step();
    }

    assert_true(val1 == 1);
    assert_true(val2 == 2);
    assert_true(val == 2);
}

template<typename TPoller>
void test_read_write_full(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    int port = GetFreePort();
    std::vector<char> data(1024*1024);
    int cur = 0;
    for (auto& ch : data) {
        ch = cur + 'a';
        cur = (cur + 1) % ('z' - 'a' + 1);
    }

    TLoop loop;
    TAddress saddr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), saddr.Domain());
    socket.Bind(saddr);
    socket.Listen();

    TAddress caddr{"127.0.0.1", port};
    TSocket client(loop.Poller(), caddr.Domain());

    TFuture<void> h1 = [](TSocket& client, TAddress& caddr, const std::vector<char>& data) -> TFuture<void>
    {
        co_await client.Connect(caddr);
        co_await TByteWriter(client).Write(data.data(), data.size());
        co_return;
    }(client, caddr, data);

    std::vector<char> received(1024*1024);
    TFuture<void> h2 = [](TSocket& server, std::vector<char>& received) -> TFuture<void>
    {
        auto client = std::move(co_await server.Accept());
        co_await TByteReader(client).Read(received.data(), received.size());
        co_return;
    }(socket, received);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_memory_equal(data.data(), received.data(), data.size());
}

template<typename TPoller>
void test_read_until(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    int port = GetFreePort();
    std::string data = R"__(line1
line2
line3
line4
line9
)__";

    TLoop loop;
    TAddress saddr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), saddr.Domain());
    socket.Bind(saddr);
    socket.Listen();

    TAddress caddr{"127.0.0.1", port};
    TSocket client(loop.Poller(), caddr.Domain());

    TFuture<void> h1 = [](TSocket& client, TAddress& caddr, const auto& data) -> TFuture<void>
    {
        co_await client.Connect(caddr);
        co_await TByteWriter(client).Write(data.data(), data.size());
        co_return;
    }(client, caddr, data);

    std::vector<std::string> received;
    TFuture<void> h2 = [](TSocket& server, auto& received) -> TFuture<void>
    {
        auto client = std::move(co_await server.Accept());
        auto reader = TByteReader(client);
        auto line1 = co_await reader.ReadUntil("\n");
        auto line2 = co_await reader.ReadUntil("\n");
        char byte;
        co_await reader.Read(&byte, 1);
        auto line3 = co_await reader.ReadUntil("\n");
        received.emplace_back(std::move(line1));
        received.emplace_back(std::move(line2));
        received.emplace_back(std::move(line3));
        co_return;
    }(socket, received);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_true(received.size() == 3);
    assert_true(received[0] == "line1\n");
    assert_true(received[1] == "line2\n");
    assert_true(received[2] == "ine3\n");
}

template<typename TPoller>
void test_read_write_struct(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    struct Test {
        std::array<char, 1024> data;
    };
    Test data;

    int cur = 0;
    for (auto& ch : data.data) {
        ch = cur + 'a';
        cur = (cur + 1) % ('z' - 'a' + 1);
    }

    int port = GetFreePort();
    TLoop loop;
    TAddress saddr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), saddr.Domain());
    socket.Bind(saddr);
    socket.Listen();

    TAddress caddr{"127.0.0.1", port};
    TSocket client(loop.Poller(), caddr.Domain());

    TFuture<void> h1 = [](TSocket& client, TAddress& caddr, auto& data) -> TFuture<void>
    {
        co_await client.Connect(caddr);
        co_await TByteWriter(client).Write(&data, data.data.size());
        co_return;
    }(client, caddr, data);

    Test received;
    TFuture<void> h2 = [](TSocket& server, auto& received) -> TFuture<void>
    {
        auto client = std::move(co_await server.Accept());
        received = co_await TStructReader<Test, TSocket>(client).Read();
        co_return;
    }(socket, received);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_memory_equal(data.data.data(), received.data.data(), data.data.size());
}

template<typename TPoller>
void test_read_write_lines(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    uint32_t seed = 31337;

    std::vector<std::string> lines;
    for (int i = 0; i < 10; i++) {
        int len = rand_(&seed) % 16 + 1;
        int letter = 'a' + i % ('z' - 'a' + 1);
        std::string line(len, letter); line.back() = '\n';
        lines.emplace_back(std::move(line));
    }

    int port = GetFreePort();
    TLoop loop;
    TAddress saddr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), saddr.Domain());
    socket.Bind(saddr);
    socket.Listen();

    TFuture<void> h1 = [](auto& poller, auto& lines, int port) -> TFuture<void>
    {
        TAddress caddr{"127.0.0.1", port};
        TSocket client(poller, caddr.Domain());
        co_await client.Connect(caddr);
        for (auto& line : lines) {
            co_await TByteWriter(client).Write(line.data(), line.size());
        }
        co_return;
    }(loop.Poller(), lines, port);

    std::vector<std::string> received;
    TFuture<void> h2 = [](TSocket& server, auto& received) -> TFuture<void>
    {
        auto client = std::move(co_await server.Accept());
        auto reader = TLineReader<TSocket>(client, 16);
        TLine line;
        do {
            line = co_await reader.Read();
            if (line) {
                std::string s; s = line.Part1; s += line.Part2;
                received.emplace_back(std::move(s));
            }
        } while (line);
        co_return;
    }(socket, received);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_int_equal(lines.size(), received.size());
    for (int i = 0; i < lines.size(); i++) {
        assert_string_equal(lines[i].data(), received[i].data());
    }
}

void test_line_splitter(void**) {
    TLineSplitter splitter(16);
    uint32_t seed = 31337;
    for (int i = 0; i < 10000; i++) {
        int len = rand_(&seed) % 16 + 1;
        int letter = 'a' + i % ('z' - 'a' + 1);
        std::string line(len, letter); line.back() = '\n';
        splitter.Push(line.data(), len);
        auto l = splitter.Pop();
        std::string result = std::string(l.Part1);
        result += l.Part2;
        assert_string_equal(line.data(), result.data());
    }

    for (int i = 0; i < 10000; i++) {
        std::vector<std::string> lines;

        int total = 0;
        while (1) {
            int len = rand_(&seed) % 6 + 1;
            total += len; if (total > 16) break;
            int letter = 'a' + i % ('z' - 'a' + 1);
            std::string line(len, letter); line.back() = '\n';
            splitter.Push(line.data(), len);
            lines.push_back(line);
        }

        for (int i = 0; i < lines.size(); i++) {
            auto l = splitter.Pop();
            std::string result = std::string(l.Part1);
            result += l.Part2;
            assert_string_equal(lines[i].data(), result.data());
        }
    }
}

void test_zero_copy_line_splitter(void**) {
    TZeroCopyLineSplitter splitter(16);
    uint32_t seed = 31337;
    for (int i = 0; i < 1000; i++) {
        int len = rand_(&seed) % 16 + 1;
        int letter = 'a' + i % ('z' - 'a' + 1);
        std::string line(len, letter); line.back() = '\n';
        splitter.Push(line.data(), len);
        auto l = splitter.Pop();
        std::string result = std::string(l.Part1);
        result += l.Part2;
        assert_string_equal(line.data(), result.data());
    }

    for (int i = 0; i < 10000; i++) {
        std::vector<std::string> lines;

        int total = 0;
        while (1) {
            int len = rand_(&seed) % 6 + 1;
            total += len; if (total > 16) break;
            int letter = 'a' + i % ('z' - 'a' + 1);
            std::string line(len, letter); line.back() = '\n';
            splitter.Push(line.data(), len);
            lines.push_back(line);
        }

        for (int i = 0; i < lines.size(); i++) {
            auto l = splitter.Pop();
            std::string result = std::string(l.Part1);
            result += l.Part2;
            assert_string_equal(lines[i].data(), result.data());
        }
    }
}

void test_line_splitter_wrap(void**) {
    TLineSplitter splitter(4); // Cap=8
    splitter.Push("aaa\n", 4);
    splitter.Pop();
    splitter.Push("bbb\n", 4);
    splitter.Pop(); // RPos hits Cap without fix

    splitter.Push("cc\n", 3);
    auto l = splitter.Pop();
    assert_true(static_cast<bool>(l));
    std::string result = std::string(l.Part1) + std::string(l.Part2);
    assert_string_equal("cc\n", result.data());
}

void test_zero_copy_line_splitter_wrap(void**) {
    TZeroCopyLineSplitter splitter(4); // Cap=8
    splitter.Push("aaa\n", 4);
    splitter.Pop();
    splitter.Push("bbb\n", 4);
    splitter.Pop(); // RPos hits Cap without fix

    splitter.Push("cc\n", 3);
    auto l = splitter.Pop();
    assert_true(static_cast<bool>(l));
    std::string result = std::string(l.Part1) + std::string(l.Part2);
    assert_string_equal("cc\n", result.data());
}

void test_self_id(void**) {
    void* id;
    TFuture<void> h = [](void** id) -> TFuture<void> {
        *id = (co_await Self()).address();
        co_return;
    }(&id);

    assert_ptr_equal(id, h.raw().address());
}

void test_resolv_nameservers(void**) {
    std::string data = R"__(nameserver 127.0.0.1
nameserver 192.168.0.2
nameserver 127.0.0.2
    )__";
    std::istringstream iss(data);
    TResolvConf conf(iss);

    assert_int_equal(conf.Nameservers.size(), 3);

    data = "";
    iss = std::istringstream(data);
    conf = TResolvConf(iss);
    assert_int_equal(conf.Nameservers.size(), 1);
}

template<typename TPoller>
void test_resolver(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    TLoop loop;
#ifdef _WIN32
    TResolver resolver(TAddress{"8.8.8.8", 53}, loop.Poller());
#else
    TResolver resolver(loop.Poller());
#endif

    std::vector<TAddress> addresses;
    TFuture<void> h1 = [](auto& resolver, std::vector<TAddress>& addresses) -> TFuture<void> {
        addresses = co_await resolver.Resolve("www.google.com");
        //for (auto& addr : addresses) {
        //    std::cout << addr.ToString() << "\n";
        //}
        co_return;
    }(resolver, addresses);

    while (!(h1.done())) {
        loop.Step();
    }

    assert_true(!addresses.empty());
}

template<typename TPoller>
void test_resolve_bad_name(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    TLoop loop;
    TResolver resolver(loop.Poller());

    std::exception_ptr ex;
    TFuture<void> h1 = [](auto& resolver, auto& ex) -> TFuture<void> {
        try {
            co_await resolver.Resolve("bad.host.name.wtf123");
        } catch (const std::exception& ) {
            ex = std::current_exception();
        }
    }(resolver, ex);

    while (!(h1.done())) {
        loop.Step();
    }

    assert_true(!!ex);
}

#ifdef HAVE_OPENSSL
template<typename TPoller>
void test_read_write_full_ssl(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    int port = GetFreePort();
    std::vector<char> data(1024);
    int cur = 0;
    for (auto& ch : data) {
        ch = cur + 'a';
        cur = (cur + 1) % ('z' - 'a' + 1);
    }

    TLoop loop;
    TAddress saddr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), saddr.Domain());
    socket.Bind(saddr);
    socket.Listen();

    TAddress caddr{"127.0.0.1", port};
    TSocket client(loop.Poller(), caddr.Domain());

    TFuture<void> h1 = [](TSocket&& client, TAddress& caddr, const std::vector<char>& data) -> TFuture<void>
    {
        TSslContext ctx = TSslContext::Client();
        auto sslClient = TSslSocket(std::move(client), ctx);
        co_await sslClient.Connect(caddr);
        co_await TByteWriter(sslClient).Write(data.data(), data.size());
        co_return;
    }(std::move(client), caddr, data);

    std::vector<char> received(1024*1024);
    TFuture<void> h2 = [](TSocket& server, std::vector<char>& received) -> TFuture<void>
    {
        TSslContext ctx = TSslContext::ServerFromMem(testMemCert, testMemKey);
        auto client = std::move(co_await server.Accept());
        auto sslClient = TSslSocket(std::move(client), ctx);
        co_await sslClient.AcceptHandshake();
        co_await TByteReader(sslClient).Read(received.data(), received.size());
        co_return;
    }(socket, received);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_memory_equal(data.data(), received.data(), data.size());
}
#endif // HAVE_OPENSSL

template<typename TPoller>
void test_future_chaining(void**) {
    TFuture<int> intFuture = []() -> TFuture<int> {
        co_return 1;
    }();

    TFuture<double> doubleFuture = intFuture.Apply([](int value) -> double {
        return value * 1.5;
    });

    double val = -1;
    [&](TFuture<double>&& f, double* val) -> TFuture<void> {
        *val = co_await f;
    }(std::move(doubleFuture), &val);

    assert_true(std::abs(val - 1.5) < 1e-13);
}

template<typename TPoller>
void test_futures_any(void**) {
    using TLoop = TLoop<TPoller>;
    TLoop loop;

    int ok = 0;
    TFuture<void> h2 = [](TPoller& poller, int* ok) -> TFuture<void> {
        std::vector<TFuture<void>> futures;
        futures.emplace_back([](TPoller& poller) -> TFuture<void> {
            co_await poller.Sleep(std::chrono::milliseconds(100));
            co_return;
        }(poller));
        futures.emplace_back([](TPoller& poller) -> TFuture<void> {
            co_await poller.Sleep(std::chrono::milliseconds(200));
            co_return;
        }(poller));
        futures.emplace_back([](TPoller& poller) -> TFuture<void> {
            co_await poller.Sleep(std::chrono::milliseconds(201));
            co_return;
        }(poller));
        futures.emplace_back([](TPoller& poller) -> TFuture<void> {
            co_await poller.Sleep(std::chrono::milliseconds(202));
            co_return;
        }(poller));
        co_await Any(std::move(futures));
        *ok = 1;
        co_return;
    }(loop.Poller(), &ok);

    while (!h2.done()) {
        loop.Step();
    }

    while (loop.Poller().TimersSize() > 0) {
        loop.Step();
    }

    assert_true(ok == 1);
}

template<typename TPoller>
void test_futures_any_result(void**) {
    using TLoop = TLoop<TPoller>;
    TLoop loop;

    int ok = 0;
    TFuture<void> h2 = [](TPoller& poller, int* ok) -> TFuture<void> {
        std::vector<TFuture<int>> futures;
        futures.emplace_back([](TPoller& poller) -> TFuture<int> {
            co_await poller.Sleep(std::chrono::milliseconds(204));
            co_return 1;
        }(poller));
        futures.emplace_back([](TPoller& poller) -> TFuture<int> {
            co_await poller.Sleep(std::chrono::milliseconds(100));
            co_return 2;
        }(poller));
        futures.emplace_back([](TPoller& poller) -> TFuture<int> {
            co_await poller.Sleep(std::chrono::milliseconds(201));
            co_return 3;
        }(poller));
        futures.emplace_back([](TPoller& poller) -> TFuture<int> {
            co_await poller.Sleep(std::chrono::milliseconds(202));
            co_return 4;
        }(poller));
        *ok = co_await Any(std::move(futures));
        co_return;
    }(loop.Poller(), &ok);

    while (!h2.done()) {
        loop.Step();
    }

    while (loop.Poller().TimersSize() > 0) {
        loop.Step();
    }

    assert_true(ok == 2);
}

template<typename TPoller>
void test_futures_any_same_wakeup(void**) {
    using TLoop = TLoop<TPoller>;
    TLoop loop;

    int ok = 0;
    TFuture<void> h2 = [](TPoller& poller, int* ok) -> TFuture<void> {
        std::vector<TFuture<void>> futures;
        auto until = TClock::now() + std::chrono::milliseconds(100);
        futures.emplace_back([](TPoller& poller, TTime until, int* ok) -> TFuture<void> {
            co_await poller.Sleep(until);
            (*ok)++;
            co_return;
        }(poller, until, ok));
        futures.emplace_back([](TPoller& poller, TTime until, int* ok) -> TFuture<void> {
            co_await poller.Sleep(until);
            (*ok)++;
            co_return;
        }(poller, until, ok));
        futures.emplace_back([](TPoller& poller, TTime until, int* ok) -> TFuture<void> {
            co_await poller.Sleep(until);
            (*ok)++;
            co_return;
        }(poller, until, ok));
        futures.emplace_back([](TPoller& poller, TTime until, int* ok) -> TFuture<void> {
            co_await poller.Sleep(until);
            (*ok)++;
            co_return;
        }(poller, until, ok));
        co_await Any(std::move(futures));
        (*ok)++;
        co_return;
    }(loop.Poller(), &ok);

    while (!h2.done()) {
        loop.Step();
    }

    while (loop.Poller().TimersSize() > 0) {
        loop.Step();
    }

    assert_true(ok == 2);
}

template<typename TPoller>
void test_futures_all(void**) {
    std::vector<int> r;
    TFuture<void> h1 = [](std::vector<int>* r) -> TFuture<void> {
        std::vector<TFuture<int>> futures;
        futures.emplace_back([]() -> TFuture<int> { co_return 1; }());
        futures.emplace_back([]() -> TFuture<int> { co_return 2; }());
        futures.emplace_back([]() -> TFuture<int> { co_return 3; }());
        futures.emplace_back([]() -> TFuture<int> { co_return 4; }());
        *r = co_await All(std::move(futures));
        co_return;
    }(&r);

    assert_true(r == (std::vector<int>{1, 2, 3, 4}));

    int ok = 0;
    TFuture<void> h2 = [](int* ok) -> TFuture<void> {
        std::vector<TFuture<void>> futures;
        futures.emplace_back([]() -> TFuture<void> { co_return; }());
        futures.emplace_back([]() -> TFuture<void> { co_return; }());
        futures.emplace_back([]() -> TFuture<void> { co_return; }());
        futures.emplace_back([]() -> TFuture<void> { co_return; }());
        co_await All(std::move(futures));
        *ok = 1;
        co_return;
    }(&ok);

    assert_true(ok == 1);
}

#ifdef __linux__

namespace {

struct TTestSuspendPromise;

struct TTestSuspendTask : std::coroutine_handle<TTestSuspendPromise>
{
    using promise_type = TTestSuspendPromise;
};

struct TTestSuspendPromise
{
    TTestSuspendTask get_return_object() { return { TTestSuspendTask::from_promise(*this) }; }
    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

} // namespace

#ifdef HAVE_URING
void test_uring_create(void**) {
    TUring uring(256);
}

void test_uring_write(void**) {
    TUring uring(256);
    char buf[1] = {'e'};
    char rbuf[1] = {'k'};
    int p[2];
    assert_int_equal(0, pipe(p));
    uring.Write(p[1], buf, 1, nullptr);
    assert_int_equal(uring.Wait(), 1);
    int err = read(p[0], rbuf, 1);
    assert_true(rbuf[0] == 'e');
}

void test_uring_read(void**) {
    TUring uring(256);
    char buf[1] = {'e'};
    char rbuf[1] = {'k'};
    int p[2];
    assert_int_equal(0, pipe(p));
    assert_int_equal(1, write(p[1], buf, 1));
    uring.Read(p[0], rbuf, 1, nullptr);
    assert_int_equal(uring.Wait(), 1);
    assert_true(rbuf[0] == 'e');
}

void test_uring_read_more_than_write(void**) {
    TUring uring(256);
    char buf[1] = {'e'};
    char rbuf[10] = "test test";
    int p[2];
    assert_int_equal(0, pipe(p));
    assert_int_equal(1, write(p[1], buf, 1));
    TFuture<void> h = []() -> TFuture<void> { co_return; }();
    uring.Read(p[0], rbuf, sizeof(rbuf), h.raw());
    assert_int_equal(uring.Wait(), 1);
    assert_int_equal(uring.Result(), 1);
    assert_true(rbuf[0] == 'e');
}

void test_uring_write_resume(void**) {
    TUring uring(256);
    char buf[1] = {'e'};
    char rbuf[1] = {'k'};
    int p[2];
    assert_int_equal(0, pipe(p));
    int r = 31337;
    TFuture<void> h = [](TUring* uring, int* r) -> TFuture<void> {
        co_await std::suspend_always();
        *r = uring->Result();
        co_return;
    }(&uring, &r);
    uring.Write(p[1], buf, 1, h.raw());
    assert_true(!h.done());
    assert_int_equal(uring.Wait(), 1);
    uring.WakeupReadyHandles();
    int err = read(p[0], rbuf, 1);
    assert_true(rbuf[0] == 'e');
    assert_int_equal(r, 1);
    assert_true(h.done());
}

void test_uring_read_resume(void**) {
    TUring uring(256);
    char buf[1] = {'e'};
    char rbuf[1] = {'k'};
    int p[2];
    assert_int_equal(0, pipe(p));
    int r = 31337;
    TFuture<void> h = [](TUring* uring, int* r) -> TFuture<void> {
        co_await std::suspend_always();
        *r = uring->Result();
        co_return;
    }(&uring, &r);
    assert_int_equal(1, write(p[1], buf, 1));
    uring.Read(p[0], rbuf, 1, h.raw());
    assert_true(!h.done());
    assert_int_equal(uring.Wait(), 1);
    uring.WakeupReadyHandles();
    assert_true(rbuf[0] == 'e');
    assert_int_equal(r, 1);
    assert_true(h.done());
}

void test_uring_file_offset(void**) {
    TUring uring(256);
    char path[] = "/tmp/coroio_uring_offset_XXXXXX";
    int fd = mkstemp(path);
    assert_true(fd >= 0);
    unlink(path);

    TFuture<void> h = []() -> TFuture<void> { co_return; }();
    const char data[] = "0123456789abcdefghij";
    for (int i = 0; i < 20; i += 5) {
        uring.Write(fd, data + i, 5, h.raw());
        assert_int_equal(uring.Wait(), 1);
        assert_int_equal(uring.Result(), 5);
    }

    char check[sizeof(data)] = {};
    assert_int_equal(20, pread(fd, check, 20, 0));
    assert_memory_equal(check, data, 20);

    assert_int_equal(0, lseek(fd, 0, SEEK_SET));
    char rbuf[20] = {};
    for (int i = 0; i < 20; i += 5) {
        uring.Read(fd, rbuf + i, 5, h.raw());
        assert_int_equal(uring.Wait(), 1);
        assert_int_equal(uring.Result(), 5);
    }
    assert_memory_equal(rbuf, data, 20);
    close(fd);
}

void test_uring_no_sqe(void** ) {
    TUring uring(1);
    char rbuf[1] = {'k'};
    int p[2]; assert_int_equal(0, pipe(p));
    assert_int_equal(1, write(p[1], rbuf, 1));
    assert_int_equal(1, write(p[1], rbuf, 1));
    uring.Read(p[0], rbuf, 1, nullptr);
    uring.Read(p[0], rbuf, 1, nullptr);
    int k = uring.Wait();
    assert_true(k == 1 || k == 2);
    if (k == 1) {
        assert_int_equal(1, uring.Wait());
    }
}
#endif

template<typename TPoller>
void test_remote_disconnect(void**) {
    bool changed = false;
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = GetFreePort();
    TLoop loop;
    TAddress saddr{"127.0.0.1", port};
    TSocket socket(loop.Poller(), saddr.Domain());
    socket.Bind(saddr);
    socket.Listen();

    TFuture<void> h1 = [](TPoller& poller, bool *changed, int port) -> TFuture<void>
    {
        TAddress addr{"127.0.0.1", port};
        auto clientSocket = TSocket(poller, addr.Domain());
        co_await clientSocket.Connect(addr);
        co_await clientSocket.Monitor();
        *changed = true;
        co_return;
    }(loop.Poller(), &changed, port);

    TFuture<void> h2 = [](TSocket* socket) -> TFuture<void>
    {
        auto clientSocket = co_await socket->Accept();
        clientSocket.Close();
        co_return;
    }(&socket);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }

    assert_true(changed);
}

/* temporary disable
void test_uring_cancel(void** ) {
    TUring uring(16);

    char rbuf[1] = {'k'};
    int p[2]; assert_int_equal(0, pipe(p));
    assert_int_equal(1, write(p[1], rbuf, 1));
    assert_int_equal(1, write(p[1], rbuf, 1));
    TTestSuspendTask h = []() -> TTestSuspendTask { co_return; }();
    uring.Read(p[0], rbuf, 1, h);
    uring.Cancel(h);
    assert_int_equal(1, uring.Wait());
    assert_true(rbuf[0] == 'k');
    h.destroy();
}
*/

#endif

namespace {

template<typename TPoller>
void StepUntilDone(TLoop<TPoller>& loop, TFuture<void>& future) {
    const auto deadline = TClock::now() + std::chrono::seconds(5);
    while (!future.done() && TClock::now() < deadline) {
        loop.Step();
    }
    assert_true(future.done());
    future.await_resume();
}

template<typename TSocket>
TFuture<void> AcceptInto(TSocket& listener, std::optional<TSocket>* accepted) {
    accepted->emplace(co_await listener.Accept());
}

template<typename TFile>
TFuture<void> ReadInto(TFile& file, char* buf, size_t size, int* got) {
    *got = co_await file.ReadSome(buf, size);
}

template<typename TFile>
TFuture<void> WriteFrom(TFile& file, const char* buf, size_t size, int* put) {
    *put = co_await file.WriteSome(buf, size);
}

// Stops after the first read, waiting in the second.
template<typename TSocket>
TFuture<void> ReadTwice(TSocket& socket, char* buf, size_t size, int* got) {
    *got = co_await socket.ReadSome(buf, size);
    *got = co_await socket.ReadSome(buf, size);
}

template<typename TSocket>
TFuture<void> ReadThenDestroy(TSocket& socket, char* buf, size_t size, std::optional<TFuture<void>>* first, std::optional<TFuture<void>>* second) {
    co_await socket.ReadSome(buf, size);
    first->reset();
    second->reset();
}

// Collects the non-zero bytes until the last one.
template<typename TSocket>
TFuture<void> ReadMarkers(TSocket& socket, char last, std::string* markers) {
    char buf[4096];
    while (markers->empty() || markers->back() != last) {
        int n = co_await socket.ReadSome(buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            if (buf[i]) {
                markers->push_back(buf[i]);
            }
        }
    }
}

template<typename TPoller>
std::pair<typename TPoller::TSocket, typename TPoller::TSocket>
ConnectedPair(TLoop<TPoller>& loop, typename TPoller::TSocket& listener, const TAddress& address) {
    using TSocket = typename TPoller::TSocket;
    TSocket client(loop.Poller(), address.Domain());
    std::optional<TSocket> server;
    auto accept = AcceptInto(listener, &server);
    auto connect = client.Connect(address);
    StepUntilDone(loop, connect);
    StepUntilDone(loop, accept);
    return {std::move(client), std::move(*server)};
}

} // namespace

// For pollers that perform the I/O themselves (io_uring, IOCP).
template<typename TPoller>
void test_destroyed_driven_reader_not_resumed(void**) {
    using TSocket = typename TPoller::TSocket;
    TLoop<TPoller> loop;
    TAddress address{"127.0.0.1", GetFreePort()};
    TSocket listener(loop.Poller(), address.Domain());
    listener.Bind(address);
    listener.Listen();
    std::vector<std::pair<TSocket, TSocket>> pairs; // client, server
    pairs.reserve(4);
    for (int i = 0; i < 4; i++) {
        pairs.emplace_back(ConnectedPair(loop, listener, address));
    }

    for (bool submitted : {false, true}) {
        char buf[4] = {0};
        int got = -1;
        std::optional<TFuture<void>> reader{ReadInto(pairs[0].second, buf, sizeof(buf), &got)};
        if (submitted) {
            loop.Step();
        }
        reader.reset();
        assert_int_equal(send(pairs[0].first.Fd(), "x", 1, 0), 1);
        for (int i = 0; i < 3; i++) {
            loop.Step();
        }
        // The read was cancelled, not completed into buf: the byte is left for the next reader.
        assert_int_equal(got, -1);
        assert_int_equal(buf[0], 0);
        auto next = ReadInto(pairs[0].second, buf, sizeof(buf), &got);
        StepUntilDone(loop, next);
        assert_int_equal(got, 1);
        assert_int_equal(buf[0], 'x');
    }

    for (bool collected : {true, false}) {
        // Destroyed after the reads complete, with the completions collected or not:
        // they are dropped, and the next one still gets its own result.
        char bufs[3][4] = {};
        int got[3] = {-1, -1, -1};
        std::optional<TFuture<void>> readers[3];
        for (int i = 0; i < 3; i++) {
            readers[i].emplace(ReadInto(pairs[i].second, bufs[i], sizeof(bufs[i]), &got[i]));
        }
        loop.Step();
        assert_int_equal(send(pairs[0].first.Fd(), "x", 1, 0), 1);
        assert_int_equal(send(pairs[1].first.Fd(), "yy", 2, 0), 2);
        assert_int_equal(send(pairs[2].first.Fd(), "zzz", 3, 0), 3);
        if (collected) {
            loop.Poller().Poll();
        }
        readers[0].reset();
        readers[1].reset();
        if (collected) {
            loop.Poller().WakeupReadyHandles();
        }
        StepUntilDone(loop, *readers[2]);
        assert_int_equal(got[0], -1);
        assert_int_equal(got[1], -1);
        assert_int_equal(got[2], 3);
        assert_memory_equal(bufs[2], "zzz", 3);
    }

    {
        // Destroyed by a coroutine resumed in the same batch: one resumed before it
        // and waiting again, and one whose completion comes after it.
        char bufs[4][4] = {};
        int got[4] = {-1, -1, -1, -1};
        std::optional<TFuture<void>> again{ReadTwice(pairs[0].second, bufs[0], sizeof(bufs[0]), &got[0])};
        std::optional<TFuture<void>> after{ReadInto(pairs[2].second, bufs[2], sizeof(bufs[2]), &got[2])};
        auto destroyer = ReadThenDestroy(pairs[1].second, bufs[1], sizeof(bufs[1]), &again, &after);
        auto last = ReadInto(pairs[3].second, bufs[3], sizeof(bufs[3]), &got[3]);
        loop.Step();
        assert_int_equal(send(pairs[0].first.Fd(), "a", 1, 0), 1);
        assert_int_equal(send(pairs[1].first.Fd(), "b", 1, 0), 1);
        assert_int_equal(send(pairs[2].first.Fd(), "cc", 2, 0), 2);
        assert_int_equal(send(pairs[3].first.Fd(), "dddd", 4, 0), 4);
        StepUntilDone(loop, destroyer);
        StepUntilDone(loop, last);
        assert_false(again.has_value());
        assert_false(after.has_value());
        assert_int_equal(got[0], 1);
        assert_int_equal(got[3], 4);
        assert_memory_equal(bufs[3], "dddd", 4);
    }
}

template<typename TPoller>
void test_destroyed_driven_accept_and_connect_not_resumed(void**) {
    using TSocket = typename TPoller::TSocket;
    TLoop<TPoller> loop;
    TAddress address{"127.0.0.1", GetFreePort()};
    TSocket listener(loop.Poller(), address.Domain());
    listener.Bind(address);
    listener.Listen();

    {
        // Destroyed while waiting: the connection is left for the next Accept.
        std::optional<TSocket> accepted;
        std::optional<TFuture<void>> accept{AcceptInto(listener, &accepted)};
        loop.Step();
        accept.reset();
        TSocket client(loop.Poller(), address.Domain());
        auto connect = client.Connect(address);
        StepUntilDone(loop, connect);
        for (int i = 0; i < 3; i++) {
            loop.Step();
        }
        assert_false(accepted.has_value());
        auto next = AcceptInto(listener, &accepted);
        StepUntilDone(loop, next);
        assert_true(accepted.has_value());
    }

    {
        // Destroyed after its completion is collected: the accepted connection is closed.
        std::optional<TSocket> accepted;
        std::optional<TFuture<void>> accept{AcceptInto(listener, &accepted)};
        loop.Step();
        TSocket client(loop.Poller(), address.Domain());
        auto [rawAddr, len] = address.RawAddr();
        connect(client.Fd(), rawAddr, len); // in progress, completes without the poller
        loop.Poller().Poll();
        accept.reset();
        loop.Poller().WakeupReadyHandles();
        assert_false(accepted.has_value());
        char buf[1];
        int got = -1;
        auto eof = ReadInto(client, buf, sizeof(buf), &got);
        StepUntilDone(loop, eof);
        assert_int_equal(got, 0);
    }

    {
        // Destroyed while connecting, kept pending by a full accept queue (see
        // test_connection_timeout): neither the completion nor the deadline resumes it.
        TAddress fullAddress{"127.0.0.1", GetFreePort()};
        TSocket full(loop.Poller(), fullAddress.Domain());
        full.Bind(fullAddress);
        full.Listen(1);
        std::vector<TSocket> clients;
        clients.reserve(16); // a pending Connect refers to its socket
        std::optional<TFuture<void>> connect;
        TTime deadline;
        for (int i = 0; i < 16 && (!connect || connect->done()); i++) {
            clients.emplace_back(loop.Poller(), fullAddress.Domain());
            deadline = TClock::now() + std::chrono::seconds(1);
            connect.emplace(clients.back().Connect(fullAddress, deadline));
            for (int j = 0; j < 3 && !connect->done(); j++) {
                loop.Step();
            }
        }
        assert_false(connect->done());
        connect.reset();
        while (TClock::now() < deadline + std::chrono::milliseconds(100)) {
            loop.Step();
        }
    }
}

#ifdef HAVE_URING
// An operation not submitted yet is dropped without reaching the kernel, so
// nothing is consumed on behalf of the destroyed coroutine.
void test_uring_destroyed_before_submit(void**) {
    using TSocket = TUring::TSocket;
    TLoop<TUring> loop;
    TAddress address{"127.0.0.1", GetFreePort()};
    TSocket listener(loop.Poller(), address.Domain());
    listener.Bind(address);
    listener.Listen();
    auto [client, server] = ConnectedPair(loop, listener, address);

    assert_int_equal(send(client.Fd(), "x", 1, 0), 1);
    char buf[4] = {0};
    int got = -1;
    std::optional<TFuture<void>> reader{ReadInto(server, buf, sizeof(buf), &got)};
    reader.reset();
    auto next = ReadInto(server, buf, sizeof(buf), &got);
    StepUntilDone(loop, next);
    assert_int_equal(got, 1);
    assert_int_equal(buf[0], 'x');

    TSocket other(loop.Poller(), address.Domain());
    auto connect = other.Connect(address);
    StepUntilDone(loop, connect);
    std::optional<TSocket> accepted;
    std::optional<TFuture<void>> accept{AcceptInto(listener, &accepted)};
    accept.reset();
    auto nextAccept = AcceptInto(listener, &accepted);
    StepUntilDone(loop, nextAccept);
    assert_true(accepted.has_value());
    assert_int_equal(send(other.Fd(), "y", 1, 0), 1);
    auto check = ReadInto(*accepted, buf, sizeof(buf), &got);
    StepUntilDone(loop, check);
    assert_int_equal(got, 1);
    assert_int_equal(buf[0], 'y');
}

void test_uring_destroyed_file_reader_and_writer(void**) {
    TLoop<TUring> loop;
    int p[2];
    assert_int_equal(pipe(p), 0);
    TUring::TFileHandle in(p[0], loop.Poller());
    TUring::TFileHandle out(p[1], loop.Poller());

    // The byte comes before the read is submitted, or after.
    for (bool submitted : {false, true}) {
        char buf[4] = {0};
        int got = -1;
        std::optional<TFuture<void>> reader{ReadInto(in, buf, sizeof(buf), &got)};
        if (submitted) {
            loop.Step();
        } else {
            assert_int_equal(write(p[1], "x", 1), 1);
        }
        reader.reset();
        if (submitted) {
            assert_int_equal(write(p[1], "x", 1), 1);
        }
        for (int i = 0; i < 3; i++) {
            loop.Step();
        }
        assert_int_equal(got, -1);
        assert_int_equal(buf[0], 0);
        auto next = ReadInto(in, buf, sizeof(buf), &got);
        StepUntilDone(loop, next);
        assert_int_equal(got, 1);
        assert_int_equal(buf[0], 'x');
    }

    // Destroyed while the pipe is full: its byte is not written.
    int size = fcntl(p[1], F_GETPIPE_SZ);
    assert_true(size > 0);
    std::vector<char> fill(size, 0);
    assert_int_equal(write(p[1], fill.data(), size), size);
    int put = -1;
    std::optional<TFuture<void>> writer{WriteFrom(out, "w", 1, &put)};
    loop.Step();
    writer.reset();
    for (int done = 0; done < size; ) {
        auto n = read(p[0], fill.data(), size - done);
        assert_true(n > 0);
        done += n;
    }
    assert_int_equal(put, -1);
    auto next = WriteFrom(out, "v", 1, &put);
    StepUntilDone(loop, next);
    char buf[4] = {0};
    int got = -1;
    auto reader = ReadInto(in, buf, sizeof(buf), &got);
    StepUntilDone(loop, reader);
    assert_int_equal(got, 1);
    assert_int_equal(buf[0], 'v');
}

void test_uring_destroyed_socket_writer(void**) {
    using TSocket = TUring::TSocket;
    TLoop<TUring> loop;
    TAddress address{"127.0.0.1", GetFreePort()};
    TSocket listener(loop.Poller(), address.Domain());
    listener.Bind(address);
    listener.Listen();
    auto [client, server] = ConnectedPair(loop, listener, address);

    char fill[4096] = {0};
    while (send(client.Fd(), fill, sizeof(fill), MSG_DONTWAIT) > 0) { }
    assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    int put = -1;
    std::optional<TFuture<void>> writer{WriteFrom(client, "w", 1, &put)};
    loop.Step();
    writer.reset();

    // Only the next writer's byte follows the filler.
    std::string markers;
    auto drain = ReadMarkers(server, 'v', &markers);
    auto next = WriteFrom(client, "v", 1, &put);
    StepUntilDone(loop, next);
    StepUntilDone(loop, drain);
    assert_string_equal(markers.c_str(), "v");
}

// Completions collected by a Wait() not followed by WakeupReadyHandles() are
// kept by the next Wait(): a destroyed coroutine drops its completion there
// instead of waiting for one that will not come again.
void test_uring_destroyed_after_repeated_wait(void**) {
    using TSocket = TUring::TSocket;
    TLoop<TUring> loop;
    TAddress address{"127.0.0.1", GetFreePort()};
    TSocket listener(loop.Poller(), address.Domain());
    listener.Bind(address);
    listener.Listen();
    auto [client, server] = ConnectedPair(loop, listener, address);
    auto [otherClient, otherServer] = ConnectedPair(loop, listener, address);

    char buf[4] = {0};
    int got = -1;
    char otherBuf[4] = {0};
    int otherGot = -1;
    std::optional<TFuture<void>> reader{ReadInto(server, buf, sizeof(buf), &got)};
    auto other = ReadInto(otherServer, otherBuf, sizeof(otherBuf), &otherGot);
    loop.Step();
    assert_int_equal(send(client.Fd(), "x", 1, 0), 1);
    assert_int_equal(send(otherClient.Fd(), "yy", 2, 0), 2);
    loop.Poller().Poll();
    loop.Poller().Poll();
    reader.reset();
    loop.Poller().WakeupReadyHandles();
    StepUntilDone(loop, other);
    assert_int_equal(got, -1);
    assert_int_equal(otherGot, 2);
    assert_memory_equal(otherBuf, "yy", 2);
}

// Cancel() reaps more completions than the CQ holds; the ones of live
// coroutines are delivered by the next Wait(), each with its own result.
void test_uring_destroyed_reader_with_full_cq(void**) {
    TUring uring(4); // room for 8 completions
    constexpr int count = 16;
    std::vector<TUring::TSocket> sockets;
    sockets.reserve(count);
    int peers[count];
    for (int i = 0; i < count; i++) {
        int sp[2];
        assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
        sockets.emplace_back(sp[0], uring);
        peers[i] = sp[1];
    }
    char bufs[count][1] = {};
    int got[count];
    std::optional<TFuture<void>> readers[count];
    for (int i = 0; i < count; i++) {
        got[i] = -1;
        readers[i].emplace(ReadInto(sockets[i], bufs[i], 1, &got[i]));
    }
    uring.Wait({0, 0});
    for (int i = 0; i < count; i++) {
        char c = 'a' + i;
        assert_int_equal(write(peers[i], &c, 1), 1);
    }
    readers[count - 2].reset();
    readers[1].reset();
    for (int i = 0; i < 3; i++) {
        uring.Wait({0, 0});
        uring.WakeupReadyHandles();
    }
    for (int i = 0; i < count; i++) {
        if (i == 1 || i == count - 2) {
            assert_int_equal(got[i], -1);
        } else {
            assert_int_equal(got[i], 1);
            assert_int_equal(bufs[i][0], 'a' + i);
        }
        close(peers[i]);
    }
}
#endif

void test_base64(void**) {
    std::string data = "test string";
    std::string encoded = NNet::NUtils::Base64Encode((const unsigned char*)data.data(), data.size());
    assert_string_equal(encoded.data(), "dGVzdCBzdHJpbmc=");
    assert_int_equal(encoded.size(), 16);

    data = "test string1";
    encoded = NNet::NUtils::Base64Encode((const unsigned char*)data.data(), data.size());
    assert_string_equal(encoded.data(), "dGVzdCBzdHJpbmcx");
    assert_int_equal(encoded.size(), 16);

    data = "test string12";
    encoded = NNet::NUtils::Base64Encode((const unsigned char*)data.data(), data.size());
    assert_string_equal(encoded.data(), "dGVzdCBzdHJpbmcxMg==");
    assert_int_equal(encoded.size(), 20);
}

void test_sha1(void**) {
    std::string digest;
    std::string data = "test string";
    digest.resize(40);
    NNet::NUtils::SHA1Digest((const unsigned char*)data.data(), data.size(), (unsigned char*)digest.data());
    assert_string_equal(digest.data(), "661295c9cbf9d6b2f6428414504a8deed3020641");

    data = "test string1";
    NNet::NUtils::SHA1Digest((const unsigned char*)data.data(), data.size(), (unsigned char*)digest.data());
    assert_string_equal(digest.data(), "3567ba6828093bdf2a25c425bc3b6c21f7bfdc53");
}

#define my_unit_test(f, a) { #f "(" #a ")", f<a>, NULL, NULL, NULL }
#define my_unit_test2(f, a, b) \
    { #f "(" #a ")", f<a>, NULL, NULL, NULL }, \
    { #f "(" #b ")", f<b>, NULL, NULL, NULL }
#define my_unit_test3(f, a, b, c) \
    { #f "(" #a ")", f<a>, NULL, NULL, NULL }, \
    { #f "(" #b ")", f<b>, NULL, NULL, NULL }, \
    { #f "(" #c ")", f<c>, NULL, NULL, NULL }
#define my_unit_test4(f, a, b, c, d) \
    { #f "(" #a ")", f<a>, NULL, NULL, NULL }, \
    { #f "(" #b ")", f<b>, NULL, NULL, NULL }, \
    { #f "(" #c ")", f<c>, NULL, NULL, NULL }, \
    { #f "(" #d ")", f<d>, NULL, NULL, NULL }

#ifdef __linux__
#ifdef HAVE_URING
#define my_unit_poller(f) my_unit_test4(f, TSelect, TPoll, TEPoll, TUring)
#else
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TEPoll)
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TKqueue)
#elif defined(_WIN32)
#define my_unit_poller(f) my_unit_test4(f, TSelect, TPoll, TEPoll, TIOCp)
#else
#define my_unit_poller(f) my_unit_test2(f, TSelect, TPoll)
#endif

#ifndef _WIN32
namespace {

template<typename TPoller>
void Complete(TLoop<TPoller>& loop, TFuture<void>& wait) {
    const auto deadline = TClock::now() + std::chrono::seconds(5);
    while (!wait.done() && TClock::now() < deadline) {
        loop.Step();
    }
    assert_true(wait.done());
    wait.await_resume();
}

template<typename TPoller>
void CheckWait() {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TNotifier notifier(loop.Poller());
    auto wait = notifier.Wait();
    assert_false(wait.done());
    notifier.Notify();
    Complete(loop, wait);

    auto next = notifier.Wait();
    assert_false(next.done());
    notifier.Notify();
    Complete(loop, next);
}

void test_notifier_epoll(void**) {
#ifdef __linux__
    CheckWait<TEPoll>();
#else
    CheckWait<TDefaultPoller>();
#endif
}

void test_notifier_poll(void**) {
    CheckWait<TPoll>();
}

void test_notifier_select(void**) {
    CheckWait<TSelect>();
}

void test_notifier_notify_before_wait(void**) {
    TLoop<TDefaultPoller> loop;
    TNotifier notifier(loop.Poller());
    notifier.Notify();
    auto wait = notifier.Wait();
    assert_true(wait.done());
    wait.await_resume();
}

void test_notifier_coalesces(void**) {
    TLoop<TDefaultPoller> loop;
    TNotifier notifier(loop.Poller());
    for (int i = 0; i < 10000; ++i) {
        notifier.Notify();
    }
    auto wait = notifier.Wait();
    assert_true(wait.done());
    wait.await_resume();
    auto next = notifier.Wait();
    assert_false(next.done());
}

void test_notifier_concurrent_producers(void**) {
    TLoop<TDefaultPoller> loop;
    TNotifier notifier(loop.Poller());
    constexpr size_t producerCount = 8;
    std::barrier start(producerCount);
    std::vector<std::jthread> producers;
    for (size_t i = 0; i < producerCount; ++i) {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            for (int j = 0; j < 10000; ++j) {
                notifier.Notify();
            }
        });
    }
    producers.clear();
    auto wait = notifier.Wait();
    assert_true(wait.done());
    wait.await_resume();
    auto next = notifier.Wait();
    assert_false(next.done());
}

TFuture<void> Observe(TNotifier& notifier, const int& payload, int& observed) {
    co_await notifier.Wait();
    observed = payload;
}

void test_notifier_cross_thread_wakeup(void**) {
    TLoop<TDefaultPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(10));
    TNotifier notifier(loop.Poller());
    int payload = 0;
    int observed = 0;
    auto wait = Observe(notifier, payload, observed);
    std::jthread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        payload = 42;
        notifier.Notify();
    });
    Complete(loop, wait);
    assert_int_equal(observed, 42);
}

TFuture<void> Drain(TNotifier& notifier, const std::atomic<int>& published, int total) {
    int consumed = 0;
    while (consumed != total) {
        co_await notifier.Wait();
        consumed = published.load(std::memory_order_acquire);
    }
}

void test_notifier_notify_while_draining(void**) {
    TLoop<TDefaultPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TNotifier notifier(loop.Poller());
    constexpr int producerCount = 4;
    constexpr int perProducer = 10000;
    std::atomic<int> published = 0;
    auto wait = Drain(notifier, published, producerCount * perProducer);
    std::vector<std::jthread> producers;
    for (int i = 0; i < producerCount; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < perProducer; ++j) {
                published.fetch_add(1, std::memory_order_release);
                notifier.Notify();
                if (j % 16 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }
    Complete(loop, wait);
    producers.clear();
    assert_int_equal(published.load(), producerCount * perProducer);
}

void test_notifier_rejects_second_listener(void**) {
    TLoop<TDefaultPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TNotifier notifier(loop.Poller());
    auto first = notifier.Wait();
    auto second = notifier.Wait();
    assert_true(second.done());
    bool rejected = false;
    try {
        second.await_resume();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert_true(rejected);
    notifier.Notify();
    Complete(loop, first);
}

void test_notifier_cancel_wait(void**) {
    TLoop<TDefaultPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    TNotifier notifier(loop.Poller());
    {
        auto cancelled = notifier.Wait();
        loop.Step();
        assert_false(cancelled.done());
    }
    loop.Step();
    auto wait = notifier.Wait();
    notifier.Notify();
    Complete(loop, wait);
}

TFuture<void> CancelNotifierWait(
    TPollerBase& poller,
    std::optional<TFuture<void>>& wait)
{
    co_await poller.Sleep(TClock::now());
    wait.reset();
}

template<typename TPoller>
void test_notifier_cancel_ready_from_timer(void**) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    TNotifier notifier(loop.Poller());
    std::optional<TFuture<void>> wait{notifier.Wait()};
    auto cancel = CancelNotifierWait(loop.Poller(), wait);
    notifier.Notify();
    loop.Step();
    assert_true(cancel.done());
    cancel.await_resume();
    assert_false(wait.has_value());

    auto next = notifier.Wait();
    assert_true(next.done());
    next.await_resume();
    loop.Step();
}

template<typename TPoller>
void test_notifier_cancel_collected_event(void**) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    TNotifier notifier(loop.Poller());
    std::optional<TFuture<void>> wait{notifier.Wait()};
    notifier.Notify();
    loop.Poller().Poll();
    wait.reset();

    auto next = notifier.Wait();
    assert_true(next.done());
    next.await_resume();
    auto pending = notifier.Wait();
    loop.Poller().WakeupReadyHandles();
    assert_false(pending.done());
    notifier.Notify();
    Complete(loop, pending);
}

template<typename TPoller>
void test_notifier_destroy_before_poll(void**) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    for (bool registered : {false, true}) {
        {
            TNotifier notifier(loop.Poller());
            auto wait = notifier.Wait();
            if (registered) {
                loop.Step();
            }
        }
        loop.Step();
        TNotifier replacement(loop.Poller());
        auto wait = replacement.Wait();
        replacement.Notify();
        Complete(loop, wait);
    }
    loop.Step();
}

void SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    assert_true(flags >= 0);
    assert_int_equal(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
}

void FillSendBuffer(int fd) {
    char buf[4096] = {0};
    for (size_t size : {sizeof(buf), size_t(1)}) {
        while (write(fd, buf, size) > 0) { }
        assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

void Drain(int fd) {
    char buf[4096];
    while (read(fd, buf, sizeof(buf)) > 0) { }
}

// Gives a waiter on the other end something to read, or room to write.
void MakeReady(bool write, int peer) {
    if (write) {
        Drain(peer);
    } else {
        assert_int_equal(::write(peer, "r", 1), 1);
    }
}

template<typename TFileHandle>
TFuture<void> ReadByte(TFileHandle& handle, char* byte) {
    co_await handle.ReadSome(byte, 1);
}

template<typename TFileHandle>
TFuture<void> WriteByte(TFileHandle& handle) {
    char byte = 'w';
    co_await handle.WriteSome(&byte, 1);
}

// The inner frame, the one the poller resumes, is freed as soon as it completes.
template<typename TFileHandle>
TFuture<void> CountRead(TFileHandle& handle, char* byte, int* resumed) {
    try {
        co_await ReadByte(handle, byte);
    } catch (const std::system_error&) {
        // ECONNRESET after hangup
    }
    ++*resumed;
}

template<typename TFileHandle>
TFuture<void> CountWrite(TFileHandle& handle, int* resumed) {
    try {
        co_await WriteByte(handle);
    } catch (const std::system_error&) {
        // EPIPE after hangup
    }
    ++*resumed;
}

template<typename TPoller>
void test_destroyed_waiter_not_resumed(void**) {
    using TFileHandle = typename TPoller::TFileHandle;
    enum { PENDING, REGISTERED, COLLECTED, CANCELLED_BY_TIMER, REPLACED };
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    for (bool write : {false, true}) {
        for (int stage : {PENDING, REGISTERED, COLLECTED, CANCELLED_BY_TIMER, REPLACED}) {
            int fds[2];
            assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
            TFileHandle handle(fds[0], loop.Poller());
            SetNonBlocking(fds[1]);
            char byte = 0;
            auto wait = [&]() {
                return write ? WriteByte(handle) : ReadByte(handle, &byte);
            };
            if (write) {
                FillSendBuffer(fds[0]);
            }
            std::optional<TFuture<void>> waiter{wait()};
            assert_false(waiter->done());
            if (stage != PENDING) {
                loop.Step();
            }
            switch (stage) {
            case PENDING:
            case REGISTERED:
                waiter.reset();
                MakeReady(write, fds[1]);
                loop.Step();
                loop.Step();
                break;
            case COLLECTED:
                MakeReady(write, fds[1]);
                loop.Poller().Poll();
                waiter.reset();
                loop.Poller().WakeupReadyHandles();
                break;
            case CANCELLED_BY_TIMER: {
                auto cancel = CancelNotifierWait(loop.Poller(), waiter);
                MakeReady(write, fds[1]);
                loop.Step();
                assert_true(cancel.done());
                cancel.await_resume();
                assert_false(waiter.has_value());
                break;
            }
            case REPLACED:
                // The new waiter registers before the old one is destroyed.
                *waiter = wait();
                MakeReady(write, fds[1]);
                Complete(loop, *waiter);
                break;
            }

            // The descriptor still serves the next waiter.
            if (write) {
                FillSendBuffer(fds[0]);
            } else {
                Drain(fds[0]);
            }
            auto next = wait();
            assert_false(next.done());
            loop.Step();
            MakeReady(write, fds[1]);
            Complete(loop, next);
            close(fds[1]);
        }
    }
}

template<typename TPoller>
void test_reused_descriptor_wakes_new_waiter(void**) {
    using TFileHandle = typename TPoller::TFileHandle;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    // shared: the old description stays open in another process, like a forked child
    for (bool shared : {false, true}) {
        for (bool registered : {false, true}) {
            int oldFds[2];
            assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, oldFds), 0);
            auto oldHandle = std::make_unique<TFileHandle>(oldFds[0], loop.Poller());
            char oldByte = 0;
            int oldResumed = 0;
            auto oldWait = CountRead(*oldHandle, &oldByte, &oldResumed);
            if (registered) {
                loop.Step();
            }
            int kept = shared ? fcntl(oldFds[0], F_DUPFD, 64) : -1;
            oldHandle.reset();

            int newFds[2];
            assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, newFds), 0);
            assert_int_equal(newFds[0], oldFds[0]);
            TFileHandle newHandle(newFds[0], loop.Poller());
            char newByte = 0;
            int newResumed = 0;
            auto newWait = CountRead(newHandle, &newByte, &newResumed);
            if (shared) {
                assert_int_equal(write(oldFds[1], "o", 1), 1);
            }
            assert_int_equal(write(newFds[1], "n", 1), 1);
            Complete(loop, newWait);
            loop.Step();
            loop.Step();
            assert_int_equal(newResumed, 1);
            assert_int_equal(newByte, 'n');
            assert_int_equal(oldResumed, 0);
            assert_false(oldWait.done());

            if (kept >= 0) {
                close(kept);
            }
            close(oldFds[1]);
            close(newFds[1]);
        }
    }
}

template<typename TSocket>
TFuture<void> AcceptOne(TSocket& listener) {
    co_await listener.Accept();
}

template<typename TSocket>
TFuture<void> ConnectUntil(TSocket& socket, TAddress address, TTime deadline) {
    co_await socket.Connect(address, deadline);
}

template<typename TSocket>
TFuture<void> CountConnect(TSocket& socket, TAddress address, TTime deadline, int* resumed) {
    try {
        co_await ConnectUntil(socket, address, deadline);
    } catch (const std::system_error&) {
        // timed out
    }
    ++*resumed;
}

template<typename TPoller>
void test_destroyed_accept_and_connect_not_resumed(void**) {
    using TSocket = typename TPoller::TSocket;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    TAddress address{"127.0.0.1", GetFreePort()};
    TSocket listener(loop.Poller(), address.Domain());
    listener.Bind(address);
    listener.Listen();

    std::optional<TFuture<void>> accept{AcceptOne(listener)};
    loop.Step();
    accept.reset();
    TSocket client(loop.Poller(), address.Domain());
    std::optional<TFuture<void>> connect{ConnectUntil(client, address, TTime::max())};
    connect.reset();
    for (int i = 0; i < 3; i++) {
        loop.Step();
    }

    // The timer fires in the same iteration in which the connection is writable.
    TSocket other(loop.Poller(), address.Domain());
    int resumed = 0;
    auto timedOut = CountConnect(other, address, TClock::now(), &resumed);
    for (int i = 0; i < 3; i++) {
        loop.Step();
    }
    assert_true(timedOut.done());
    assert_int_equal(resumed, 1);
}

template<typename TPoller>
void test_reuse_after_closing_several(void**) {
    using TFileHandle = typename TPoller::TFileHandle;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    int oldFds[2][2];
    std::unique_ptr<TFileHandle> oldHandles[2];
    std::optional<TFuture<void>> oldWaits[2];
    char oldBytes[2] = {0};
    for (int i = 0; i < 2; i++) {
        assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, oldFds[i]), 0);
        oldHandles[i] = std::make_unique<TFileHandle>(oldFds[i][0], loop.Poller());
        oldWaits[i].emplace(ReadByte(*oldHandles[i], &oldBytes[i]));
    }
    loop.Step();
    // Both registrations are removed in one iteration, the later one first.
    oldHandles[1].reset();
    oldHandles[0].reset();
    loop.Step();

    int newFds[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, newFds), 0);
    TFileHandle newHandles[2] = {{newFds[0], loop.Poller()}, {newFds[1], loop.Poller()}};
    char newBytes[2] = {0};
    int resumed[2] = {0};
    TFuture<void> newWaits[2] = {
        CountRead(newHandles[0], &newBytes[0], &resumed[0]),
        CountRead(newHandles[1], &newBytes[1], &resumed[1]),
    };
    loop.Step();
    assert_int_equal(write(newFds[0], "0", 1), 1);
    assert_int_equal(write(newFds[1], "1", 1), 1);
    Complete(loop, newWaits[0]);
    Complete(loop, newWaits[1]);
    assert_int_equal(newBytes[0], '1');
    assert_int_equal(newBytes[1], '0');
    assert_false(oldWaits[0]->done());
    assert_false(oldWaits[1]->done());

    close(oldFds[0][1]);
    close(oldFds[1][1]);
}

template<typename TPoller>
void test_hangup_wakes_reader_and_writer_once(void**) {
    using TFileHandle = typename TPoller::TFileHandle;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(0));
    int fds[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    TFileHandle handle(fds[0], loop.Poller());
    FillSendBuffer(fds[0]);
    char byte = 0;
    int reads = 0;
    int writes = 0;
    auto reader = CountRead(handle, &byte, &reads);
    auto writer = CountWrite(handle, &writes);
    loop.Step();
    close(fds[1]);
    const auto deadline = TClock::now() + std::chrono::seconds(5);
    while (!(reader.done() && writer.done()) && TClock::now() < deadline) {
        loop.Step();
    }
    loop.Step();
    loop.Step();
    assert_int_equal(reads, 1);
    assert_int_equal(writes, 1);
}

template<typename TSocket>
TFuture<void> ServeRequest(TSocket client) {
    try {
        auto header = co_await TByteReader(client).ReadUntil("\r\n\r\n");
        if (header.starts_with("GET /slow")) {
            // The response comes later, e.g. from a child process.
            co_await client.Poller()->Yield();
        }
        co_await TByteWriter(client).Write("OK", 2);
    } catch (const std::exception&) {
        // the client went away
    }
}

template<typename TPoller>
TFuture<void> AcceptRequests(
    typename TPoller::TSocket& listener,
    std::list<TFuture<void>>& handlers,
    std::deque<int>& inherited)
{
    while (true) {
        auto client = co_await listener.Accept();
        // A child forked meanwhile holds the descriptor until it exits.
        inherited.push_back(dup(client.Fd()));
        handlers.push_back(ServeRequest(std::move(client)));
    }
}

template<typename TPoller>
TFuture<void> SendRequest(TPoller& poller, TAddress address, std::string path, bool abort, int* completed) {
    using TSocket = typename TPoller::TSocket;
    TSocket socket(poller, address.Domain());
    // Reset on close: no TIME_WAIT, so repeated runs do not run out of ports.
    linger reset{1, 0};
    if (setsockopt(socket.Fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
    co_await socket.Connect(address);
    // The header terminator is split between the two parts.
    std::string start = "GET " + path + " HTTP/1.1\r\nHost: x\r\n";
    co_await TByteWriter(socket).Write(start.data(), start.size());
    // Let the server accept and wait for the rest of the request.
    co_await poller.Sleep(std::chrono::milliseconds(1));
    if (abort) {
        co_return;
    }
    co_await TByteWriter(socket).Write("\r\n", 2);
    char response[2];
    co_await TByteReader(socket).Read(response, sizeof(response));
    co_await poller.Yield();
    ++*completed;
}

template<typename TPoller>
void test_short_connections(void**) {
    using TSocket = typename TPoller::TSocket;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TAddress address{"127.0.0.1", GetFreePort()};
    TSocket listener(loop.Poller(), address.Domain());
    listener.Bind(address);
    listener.Listen(1024);
    std::list<TFuture<void>> handlers;
    std::list<TFuture<void>> clients;
    std::deque<int> inherited;
    auto server = AcceptRequests<TPoller>(listener, handlers, inherited);
    auto reap = [](std::list<TFuture<void>>& futures) {
        for (auto it = futures.begin(); it != futures.end(); ) {
            if (it->done()) {
                try {
                    it->await_resume();
                } catch (const std::exception& ex) {
                    fail_msg("%s", ex.what());
                }
                it = futures.erase(it);
            } else {
                ++it;
            }
        }
    };

    uint32_t seed = 31337;
    int requests = 0;
    int started = 0;
    int completed = 0;
    const auto deadline = TClock::now() + std::chrono::seconds(5);
    while (requests < 300 && TClock::now() < deadline) {
        while (clients.size() < 8 && requests < 300) {
            auto kind = rand_(&seed) % 3;
            bool abort = kind == 0;
            requests++;
            started += !abort;
            clients.push_back(SendRequest(loop.Poller(), address, kind == 1 ? "/slow" : "/", abort, &completed));
        }
        loop.Step();
        reap(clients);
        reap(handlers);
        while (inherited.size() > 16) {
            close(inherited.front());
            inherited.pop_front();
        }
    }
    const auto drain = TClock::now() + std::chrono::seconds(5);
    while (!clients.empty() && TClock::now() < drain) {
        loop.Step();
        reap(clients);
        reap(handlers);
    }
    assert_true(clients.empty());
    assert_int_equal(completed, started);
    for (int fd : inherited) {
        close(fd);
    }
}

} // namespace
#endif // _WIN32

int main(int argc, char* argv[]) {
    TInitializer init;

    std::vector<CMUnitTest> tests;
    std::unordered_set<std::string> filters;
    tests.reserve(500);

    parse_filters(argc, argv, filters);

#ifndef _WIN32
    ADD_TEST(cmocka_unit_test, test_notifier_epoll);
    ADD_TEST(cmocka_unit_test, test_notifier_poll);
    ADD_TEST(cmocka_unit_test, test_notifier_select);
    ADD_TEST(cmocka_unit_test, test_notifier_notify_before_wait);
    ADD_TEST(cmocka_unit_test, test_notifier_coalesces);
    ADD_TEST(cmocka_unit_test, test_notifier_concurrent_producers);
    ADD_TEST(cmocka_unit_test, test_notifier_cross_thread_wakeup);
    ADD_TEST(cmocka_unit_test, test_notifier_notify_while_draining);
    ADD_TEST(cmocka_unit_test, test_notifier_rejects_second_listener);
    ADD_TEST(cmocka_unit_test, test_notifier_cancel_wait);
    ADD_TEST(my_unit_test2, test_notifier_cancel_ready_from_timer, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_notifier_cancel_collected_event, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_notifier_destroy_before_poll, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_destroyed_waiter_not_resumed, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_reused_descriptor_wakes_new_waiter, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_reuse_after_closing_several, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_destroyed_accept_and_connect_not_resumed, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_hangup_wakes_reader_and_writer_once, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_short_connections, TSelect, TPoll);
#ifdef __linux__
    ADD_TEST(my_unit_test, test_notifier_cancel_ready_from_timer, TEPoll);
    ADD_TEST(my_unit_test, test_notifier_cancel_collected_event, TEPoll);
    ADD_TEST(my_unit_test, test_notifier_destroy_before_poll, TEPoll);
    ADD_TEST(my_unit_test, test_destroyed_waiter_not_resumed, TEPoll);
    ADD_TEST(my_unit_test, test_reused_descriptor_wakes_new_waiter, TEPoll);
    ADD_TEST(my_unit_test, test_reuse_after_closing_several, TEPoll);
    ADD_TEST(my_unit_test, test_destroyed_accept_and_connect_not_resumed, TEPoll);
    ADD_TEST(my_unit_test, test_hangup_wakes_reader_and_writer_once, TEPoll);
    ADD_TEST(my_unit_test, test_short_connections, TEPoll);
#elif defined(__APPLE__) || defined(__FreeBSD__)
    ADD_TEST(my_unit_test, test_notifier_cancel_ready_from_timer, TKqueue);
    ADD_TEST(my_unit_test, test_notifier_cancel_collected_event, TKqueue);
    ADD_TEST(my_unit_test, test_notifier_destroy_before_poll, TKqueue);
    ADD_TEST(my_unit_test, test_destroyed_waiter_not_resumed, TKqueue);
    ADD_TEST(my_unit_test, test_reused_descriptor_wakes_new_waiter, TKqueue);
    ADD_TEST(my_unit_test, test_reuse_after_closing_several, TKqueue);
    ADD_TEST(my_unit_test, test_destroyed_accept_and_connect_not_resumed, TKqueue);
    ADD_TEST(my_unit_test, test_hangup_wakes_reader_and_writer_once, TKqueue);
    ADD_TEST(my_unit_test, test_short_connections, TKqueue);
#endif
#endif

    ADD_TEST(cmocka_unit_test, test_base64);
    ADD_TEST(cmocka_unit_test, test_sha1);
    ADD_TEST(cmocka_unit_test, test_addr);
    ADD_TEST(cmocka_unit_test, test_addr6);
    ADD_TEST(cmocka_unit_test, test_bad_addr);
    ADD_TEST(cmocka_unit_test, test_timespec);
    ADD_TEST(cmocka_unit_test, test_line_splitter);
    ADD_TEST(cmocka_unit_test, test_zero_copy_line_splitter);
    ADD_TEST(cmocka_unit_test, test_line_splitter_wrap);
    ADD_TEST(cmocka_unit_test, test_zero_copy_line_splitter_wrap);
    ADD_TEST(cmocka_unit_test, test_self_id);
    ADD_TEST(cmocka_unit_test, test_resolv_nameservers);
    ADD_TEST(my_unit_poller, test_listen);
    ADD_TEST(my_unit_poller, test_timeout);
    ADD_TEST(my_unit_poller, test_timeout2);
    ADD_TEST(my_unit_poller, test_accept);
    ADD_TEST(my_unit_poller, test_write_after_connect);
    ADD_TEST(my_unit_poller, test_write_after_accept);
    ADD_TEST(my_unit_poller, test_connection_timeout);
    ADD_TEST(my_unit_poller, test_connection_timeout_cleanup);
    ADD_TEST(my_unit_poller, test_remove_connection_timeout);
    ADD_TEST(my_unit_poller, test_connection_refused);
    ADD_TEST(my_unit_poller, test_connection_refused_on_write);
    ADD_TEST(my_unit_poller, test_connection_refused_on_read);
    ADD_TEST(my_unit_poller, test_read_write_same_socket);
    ADD_TEST(my_unit_poller, test_read_write_full);
    ADD_TEST(my_unit_poller, test_read_until);
    ADD_TEST(my_unit_poller, test_read_write_struct);
    ADD_TEST(my_unit_poller, test_read_write_lines);
    ADD_TEST(my_unit_poller, test_future_chaining);
    ADD_TEST(my_unit_poller, test_futures_any);
    ADD_TEST(my_unit_poller, test_futures_any_result);
    ADD_TEST(my_unit_poller, test_futures_any_same_wakeup);
    ADD_TEST(my_unit_poller, test_futures_all);
#ifndef _WIN32
    ADD_TEST(my_unit_poller, test_cloexec);
#ifdef HAVE_OPENSSL
    ADD_TEST(my_unit_test2, test_read_write_full_ssl, TSelect, TPoll);
#endif
#endif
    ADD_TEST(my_unit_test2, test_resolver, TSelect, TPoll);
    ADD_TEST(my_unit_test2, test_resolve_bad_name, TSelect, TPoll);
#ifdef __linux__
    ADD_TEST(my_unit_test2, test_remote_disconnect, TPoll, TEPoll);
#ifdef HAVE_URING
    ADD_TEST(cmocka_unit_test, test_uring_create);
    ADD_TEST(cmocka_unit_test, test_uring_write);
    ADD_TEST(cmocka_unit_test, test_uring_read);
    ADD_TEST(cmocka_unit_test, test_uring_read_more_than_write);
    ADD_TEST(cmocka_unit_test, test_uring_write_resume);
    ADD_TEST(cmocka_unit_test, test_uring_read_resume);
    ADD_TEST(cmocka_unit_test, test_uring_file_offset);
    ADD_TEST(cmocka_unit_test, test_uring_no_sqe);
    // ADD_TEST(cmocka_unit_test, test_uring_cancel); // temporary disable
    ADD_TEST(my_unit_test, test_destroyed_driven_reader_not_resumed, TUring);
    ADD_TEST(my_unit_test, test_destroyed_driven_accept_and_connect_not_resumed, TUring);
    ADD_TEST(cmocka_unit_test, test_uring_destroyed_before_submit);
    ADD_TEST(cmocka_unit_test, test_uring_destroyed_file_reader_and_writer);
    ADD_TEST(cmocka_unit_test, test_uring_destroyed_socket_writer);
    ADD_TEST(cmocka_unit_test, test_uring_destroyed_after_repeated_wait);
    ADD_TEST(cmocka_unit_test, test_uring_destroyed_reader_with_full_cq);
#endif
#endif
#ifdef _WIN32
    ADD_TEST(my_unit_test, test_destroyed_driven_reader_not_resumed, TIOCp);
    ADD_TEST(my_unit_test, test_destroyed_driven_accept_and_connect_not_resumed, TIOCp);
#endif

    return _cmocka_run_group_tests("tests", tests.data(), tests.size(), NULL, NULL);
}
