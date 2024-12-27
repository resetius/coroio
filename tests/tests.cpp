#include <chrono>
#include <array>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>

#include <coroio/all.hpp>

#include "ut.hpp"

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

int getport() {
    static int port = 8000;
    return port++;
}

} // namespace

using namespace NNet;

#ifdef __linux__
#define DISABLE_URING1 \
    if (uring.Kernel() < std::make_tuple(6, 0, 0)) { \
        std::cerr << "Temporary disable " << __FUNCTION__ << " for " << uring.KernelStr() << "\n"; \
        return; \
    } \

#define DISABLE_URING \
    if constexpr(std::is_same_v<TPoller, TUring>) { \
        TUring& uring = static_cast<TUring&>(loop.Poller()); \
        DISABLE_URING1 \
    }
#else
#define DISABLE_URING
#endif

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
    int port = getport();
    TAddress address("127.0.0.1", port);
    auto low = std::get<sockaddr_in>(address.Addr());
    assert_true(low.sin_port == ntohs(port));
    assert_true(low.sin_family == AF_INET);

    unsigned int value = ntohl((127<<24)|(0<<16)|(0<<8)|1);
    assert_true(memcmp(&low.sin_addr, &value, 4) == 0);
}

void test_addr6(void**) {
    int port = getport();
    TAddress address("::1", port);
    auto low = std::get<sockaddr_in6>(address.Addr());
    assert_true(low.sin6_port == ntohs(port));
    assert_true(low.sin6_family == AF_INET6);
}

void test_bad_addr(void**) {
    int port = getport();
    int flag = 0;
    try {
        TAddress address("wtf", port);
    } catch (const std::exception& ex) {
        flag = 1;
    }
    assert_int_equal(flag, 1);
}

template<typename TPoller>
void test_accept(void**) {
    int port = getport();
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", port}, loop.Poller());
    TSocket clientSocket{};
    socket.Bind();
    socket.Listen();

    TFuture<void> h1 = [](TPoller& poller, int port) -> TFuture<void>
    {
        try {
            TSocket client(TAddress{"127.0.0.1", port}, poller);
            co_await client.Connect();
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

    in_addr addr1 = std::get<sockaddr_in>(clientSocket.Addr().Addr()).sin_addr;
    in_addr addr2 = std::get<sockaddr_in>(socket.Addr().Addr()).sin_addr;
    assert_memory_equal(&addr1, &addr2, 4);
}

template<typename TPoller>
void test_write_after_connect(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = getport();
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", port}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[256] = {0};

    TFuture<void> h1 = [](TPoller& poller, char* buf, int size, int port) -> TFuture<void>
    {
        try {
            TSocket client(TAddress{"127.0.0.1", port}, poller);
            co_await client.Connect();
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
    int port = getport();
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", port}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[256] = {0};

    TFuture<void> h1 = [](TPoller& poller, char* buf, int size, int port) -> TFuture<void>
    {
        try {
            TSocket client(TAddress{"127.0.0.1", port}, poller);
            co_await client.Connect();
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
    int port = getport();
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", port}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char buf1[128] = {0};
    char buf2[128] = {0};

    TSocket client(TAddress{"127.0.0.1", port}, loop.Poller());

    TFuture<void> h1 = [](TSocket& client) -> TFuture<void>
    {
        co_await client.Connect();
        co_return;
    }(client);

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
    int port = getport();
    TLoop loop;
    bool timeout = false;

    TFuture<void> h = [](TPoller& poller, bool& timeout, int port) -> TFuture<void>
    {
        // TODO: use other addr
        TSocket client(TAddress{"10.0.0.1", port}, poller);
        try {
            co_await client.Connect(TClock::now()+std::chrono::milliseconds(100));
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

    assert_true(timeout);
}

template<typename TPoller>
void test_remove_connection_timeout(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = getport();
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", port}, loop.Poller());
    socket.Bind();
    socket.Listen();

    bool timeout = false;

    TFuture<void> h = [](TPoller& poller, bool& timeout, int port) -> TFuture<void>
    {
        TSocket client(TAddress{"127.0.0.1", port}, poller);
        try {
            co_await client.Connect(TClock::now()+std::chrono::milliseconds(10));
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
    int port = getport();
    TLoop loop;
    std::error_code err;

    TFuture<void> h = [](TPoller& poller, std::error_code* err, int port) -> TFuture<void>
    {
        TSocket clientSocket(TAddress{"127.0.0.1", port}, poller);
        char buffer[] = "test";
        try {
            // set timeout (windows workaround)
            co_await clientSocket.Connect(TClock::now()+std::chrono::milliseconds(100));
            co_await clientSocket.WriteSome(buffer, sizeof(buffer));
        } catch (const std::system_error& ex) {
            *err = ex.code();
        }
        co_return;
    }(loop.Poller(), &err, port);

    while (!h.done()) {
        loop.Step();
    }

    // EPIPE in MacOS
    // std::errc::timed_out for windows
    assert_true(err == std::errc::timed_out || err.value() == ECONNREFUSED || err.value() == EPIPE);
}

template<typename TPoller>
void test_connection_refused_on_read(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = getport();
    TLoop loop;
    std::error_code err;

    TFuture<void> h = [](TPoller& poller, std::error_code* err, int port) -> TFuture<void>
    {
        TSocket clientSocket(TAddress{"127.0.0.1", port}, poller);
        char buffer[] = "test";
        try {
            // set timeout (windows workaround)
            co_await clientSocket.Connect(TClock::now()+std::chrono::milliseconds(100));
            co_await clientSocket.ReadSome(buffer, sizeof(buffer));
        } catch (const std::system_error& ex) {
            *err = ex.code();
        }
        co_return;
    }(loop.Poller(), &err, port);

    while (!h.done()) {
        loop.Step();
    }

    // std::errc::timed_out for windows
    assert_true(err == std::errc::timed_out || err.value() == ECONNREFUSED);
}

template<typename TPoller>
void test_resolver(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    TLoop loop;
#ifdef _WIN32
    TResolver<TPollerBase> resolver(TAddress{"8.8.8.8", 53}, loop.Poller());
#else
    TResolver<TPollerBase> resolver(loop.Poller());
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
    TResolver<TPollerBase> resolver(loop.Poller());

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

template<typename TPoller>
void test_read_write_full_ssl(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;

    int port = getport();
    std::vector<char> data(1024);
    int cur = 0;
    for (auto& ch : data) {
        ch = cur + 'a';
        cur = (cur + 1) % ('z' - 'a' + 1);
    }

    TLoop loop;
    TSocket socket(NNet::TAddress{"127.0.0.1", port}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TSocket client(NNet::TAddress{"127.0.0.1", port}, loop.Poller());

    TFuture<void> h1 = [](TSocket&& client, const std::vector<char>& data) -> TFuture<void>
    {
        TSslContext ctx = TSslContext::Client();
        auto sslClient = TSslSocket(std::move(client), ctx);
        co_await sslClient.Connect();
        co_await TByteWriter(sslClient).Write(data.data(), data.size());
        co_return;
    }(std::move(client), data);

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

template<typename TPoller>
void test_remote_disconnect(void**) {
    bool changed = false;
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    int port = getport();
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", port}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TFuture<void> h1 = [](TPoller& poller, bool *changed, int port) -> TFuture<void>
    {
        auto clientSocket = TSocket(TAddress{"127.0.0.1", port}, poller);
        co_await clientSocket.Connect();
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
    DISABLE_URING1

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
#define my_unit_poller(f) my_unit_test4(f, TSelect, TPoll, TEPoll, TUring)
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TKqueue)
#elif defined(_WIN32)
#define my_unit_poller(f) my_unit_test4(f, TSelect, TPoll, TEPoll, TIOCp)
#else
#define my_unit_poller(f) my_unit_test2(f, TSelect, TPoll)
#endif

namespace ut = boost::ut;

ut::suite<"basic"> basic = []{
    using namespace boost::ut;

    "addr"_test = [] {
        int port = getport();
        TAddress address("127.0.0.1", port);
        auto low = std::get<sockaddr_in>(address.Addr());
        expect(low.sin_port == ntohs(port));
        expect(low.sin_family == AF_INET);

        unsigned int value = ntohl((127<<24)|(0<<16)|(0<<8)|1);
        expect(memcmp(&low.sin_addr, &value, 4) == 0);
    };

    "addr6"_test = [] {
        int port = getport();
        TAddress address("::1", port);
        auto low = std::get<sockaddr_in6>(address.Addr());
        expect(low.sin6_port == ntohs(port));
        expect(low.sin6_family == AF_INET6);
    };

    "bad_addr"_test = [] {
        int port = getport();
        int flag = 0;
        try {
            TAddress address("wtf", port);
        } catch (const std::exception& ex) {
            flag = 1;
        }
        expect(eq(flag, 1));
    };

    "timespec"_test = [] {
        auto t1 =  std::chrono::seconds(4);
        auto t2 =  std::chrono::seconds(10);
        auto ts = GetTimespec(TTime(t1), TTime(t2), maxDiration);
        expect(eq(ts.tv_sec, 6));
        expect(eq(ts.tv_nsec, 0));

        auto t3 =  std::chrono::milliseconds(10001);
        ts = GetTimespec(TTime(t1), TTime(t3), maxDiration);
        expect(eq(ts.tv_sec, 6));
        expect(eq(ts.tv_nsec, 1000*1000));

        auto t4 = std::chrono::minutes(10000);
        ts = GetTimespec(TTime(t1), TTime(t4), maxDiration);
        expect(eq(ts.tv_sec, 10));
        expect(eq(ts.tv_nsec, 0));
    };

    "line_splitter"_test = [] {
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
            expect(eq(line, result));
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
                expect(eq(lines[i], result));
            }
        }
    };

    "zero_copy_line_splitter"_test = [] {
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
            expect(eq(line, result));
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
                expect(eq(lines[i], result));
            }
        }
    };

    "self_id"_test = [] {
        void* id;
        TFuture<void> h = [](void** id) -> TFuture<void> {
            *id = (co_await Self()).address();
            co_return;
        }(&id);

        expect(eq(id, h.raw().address()));
    };

    "resolv_nameservers"_test = [] {
        std::string data = R"__(nameserver 127.0.0.1
    nameserver 192.168.0.2
    nameserver 127.0.0.2
        )__";
        std::istringstream iss(data);
        TResolvConf conf(iss);

        expect(eq(conf.Nameservers.size(), 3));

        data = "";
        iss = std::istringstream(data);
        conf = TResolvConf(iss);
        expect(eq(conf.Nameservers.size(), 1));
    };
};


// Helper function to concatenate test name and type name
constexpr auto concatenate_test_name(std::string_view name, std::string_view type_name) {
    return std::string{name} + "(" + std::string{type_name} + ")";
}

// Primary template for recursive test generation
template <typename... Types>
struct Test;

// Specialization for at least one type
template <typename First, typename... Rest>
struct Test<First, Rest...> {
    template <typename Function>
    static constexpr void Generate(const std::string& name, Function&& func) {
        using namespace boost::ut;

        // Create test for the current type
        test(concatenate_test_name(name, boost::ut::reflection::type_name<First>())) = [&] () {
            func.template operator()<First>();
        };

        // Recurse to the next types
        Test<Rest...>::Generate(name, std::forward<Function>(func));
    }
};

// Base case for no types
template <>
struct Test<> {
    template <typename Function>
    static constexpr void Generate(const std::string&, Function&&) {
        // End of recursion, do nothing
    }
};


ut::suite<"socket"> socket_suite = [] {
    using namespace boost::ut;
    using Pollers = Test<TSelect,TPoll,TKqueue>;

    auto test_listen = [] <typename TPoller> {
        int port = getport();
        TLoop<TPoller> loop;
        TAddress address("127.0.0.1", port);
        TSocket socket(std::move(address), loop.Poller());
        socket.Bind();
        socket.Listen();
        expect(true);
    };

    Pollers::Generate("listen", test_listen);

    auto test_timeout = [] <typename TPoller> {
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

        expect(next >= now + timeout);
    };

    Pollers::Generate("timeout", test_timeout);

    auto test_timeout2 = [&] <typename TPoller> {
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

        expect(eq(val1, 1));
        expect(eq(val2, 2));
        expect(eq(val, 2));
    };

    Pollers::Generate("timeout2", test_timeout2);

    auto test_read_write_full = [&] <typename TPoller> {
        using TLoop = TLoop<TPoller>;
        using TSocket = typename TPoller::TSocket;

        int port = getport();
        std::vector<char> data(1024*1024);
        int cur = 0;
        for (auto& ch : data) {
            ch = cur + 'a';
            cur = (cur + 1) % ('z' - 'a' + 1);
        }

        TLoop loop;
        TSocket socket(NNet::TAddress{"127.0.0.1", port}, loop.Poller());
        socket.Bind();
        socket.Listen();

        TSocket client(NNet::TAddress{"127.0.0.1", port}, loop.Poller());

        TFuture<void> h1 = [](TSocket& client, const std::vector<char>& data) -> TFuture<void>
        {
            co_await client.Connect();
            co_await TByteWriter(client).Write(data.data(), data.size());
            co_return;
        }(client, data);

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

        expect(data == received);
    };

    Pollers::Generate("read_write_full", test_read_write_full);

    auto test_read_write_struct = [&] <typename TPoller> {
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

        int port = getport();
        TLoop loop;
        TSocket socket(NNet::TAddress{"127.0.0.1", port}, loop.Poller());
        socket.Bind();
        socket.Listen();

        TSocket client(NNet::TAddress{"127.0.0.1", port}, loop.Poller());

        TFuture<void> h1 = [](TSocket& client, auto& data) -> TFuture<void>
        {
            co_await client.Connect();
            co_await TByteWriter(client).Write(&data, data.data.size());
            co_return;
        }(client, data);

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

        expect(data.data == received.data);
    };

    Pollers::Generate("read_write_struct", test_read_write_struct);

    auto test_read_write_lines = [&] <typename TPoller> {
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

        int port = getport();
        TLoop loop;
        TSocket socket(NNet::TAddress{"127.0.0.1", port}, loop.Poller());
        socket.Bind();
        socket.Listen();

        TFuture<void> h1 = [](auto& poller, auto& lines, int port) -> TFuture<void>
        {
            TSocket client(NNet::TAddress{"127.0.0.1", port}, poller);
            co_await client.Connect();
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
    };

    Pollers::Generate("read_write_lines", test_read_write_lines);
};

int main(int argc, const char** argv) {
    TInitializer init;
    return ut::cfg<>.run({.argc = argc, .argv = argv});

#if 0
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_addr),
        cmocka_unit_test(test_addr6),
        cmocka_unit_test(test_bad_addr),
        cmocka_unit_test(test_timespec),
        cmocka_unit_test(test_line_splitter),
        cmocka_unit_test(test_zero_copy_line_splitter),
        cmocka_unit_test(test_self_id),
        cmocka_unit_test(test_resolv_nameservers),
        my_unit_poller(test_listen),
        my_unit_poller(test_timeout),
        my_unit_poller(test_timeout2),
        my_unit_poller(test_accept),
        my_unit_poller(test_write_after_connect),
        my_unit_poller(test_write_after_accept),
        my_unit_poller(test_connection_timeout),
        my_unit_poller(test_remove_connection_timeout),
        my_unit_poller(test_connection_refused_on_write),
        my_unit_poller(test_connection_refused_on_read),
        my_unit_poller(test_read_write_same_socket),
        my_unit_poller(test_read_write_full),
        my_unit_poller(test_read_write_struct),
        my_unit_poller(test_read_write_lines),
        my_unit_poller(test_future_chaining),
        my_unit_poller(test_futures_any),
        my_unit_poller(test_futures_any_result),
        my_unit_poller(test_futures_any_same_wakeup),
        my_unit_poller(test_futures_all),
#ifndef _WIN32
        my_unit_test2(test_read_write_full_ssl, TSelect, TPoll),
#endif
        my_unit_test2(test_resolver, TSelect, TPoll),
        my_unit_test2(test_resolve_bad_name, TSelect, TPoll),
#ifdef __linux__
        my_unit_test2(test_remote_disconnect, TPoll, TEPoll),
        cmocka_unit_test(test_uring_create),
        cmocka_unit_test(test_uring_write),
        cmocka_unit_test(test_uring_read),
        cmocka_unit_test(test_uring_read_more_than_write),
        cmocka_unit_test(test_uring_write_resume),
        cmocka_unit_test(test_uring_read_resume),
        cmocka_unit_test(test_uring_no_sqe),
        // cmocka_unit_test(test_uring_cancel), // temporary disable
#endif
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
