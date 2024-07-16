#include <chrono>
#include <array>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>

#include <coroio/all.hpp>

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
    TAddress address("127.0.0.1", 8888);
    auto low = std::get<sockaddr_in>(address.Addr());
    assert_true(low.sin_port == ntohs(8888));
    assert_true(low.sin_family == AF_INET);

    unsigned int value = ntohl((127<<24)|(0<<16)|(0<<8)|1);
    assert_true(memcmp(&low.sin_addr, &value, 4) == 0);
}

void test_addr6(void**) {
    TAddress address("::1", 8888);
    auto low = std::get<sockaddr_in6>(address.Addr());
    assert_true(low.sin6_port == ntohs(8888));
    assert_true(low.sin6_family == AF_INET6);
}

void test_bad_addr(void**) {
    int flag = 0;
    try {
        TAddress address("wtf", 8888);
    } catch (const std::exception& ex) {
        flag = 1;
    }
    assert_int_equal(flag, 1);
}

template<typename TPoller>
void test_listen(void**) {
    TLoop<TPoller> loop;
    TAddress address("127.0.0.1", 8888);
    TSocket socket(std::move(address), loop.Poller());
    socket.Bind();
    socket.Listen();
}

template<typename TPoller>
void test_accept(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    TSocket clientSocket{};
    socket.Bind();
    socket.Listen();

    TFuture<void> h1 = [](TPoller& poller) -> TFuture<void>
    {
        TSocket client(TAddress{"127.0.0.1", 8888}, poller);
        co_await client.Connect();
        co_return;
    }(loop.Poller());

    TFuture<void> h2 = [](TSocket* socket, TSocket* clientSocket) -> TFuture<void>
    {
        *clientSocket = std::move(co_await socket->Accept());
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
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", 8898}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[256] = {0};

    TFuture<void> h1 = [](TPoller& poller, char* buf, int size) -> TFuture<void>
    {
        TSocket client(TAddress{"127.0.0.1", 8898}, poller);
        co_await client.Connect();
        co_await client.WriteSome(buf, size);
        co_return;
    }(loop.Poller(), send_buf, sizeof(send_buf));

    TFuture<void> h2 = [](TSocket* socket, char* buf, int size) -> TFuture<void>
    {
        TSocket clientSocket = std::move(co_await socket->Accept());
        co_await clientSocket.ReadSome(buf, size);
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
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[256] = {0};

    TFuture<void> h1 = [](TPoller& poller, char* buf, int size) -> TFuture<void>
    {
        TSocket client(TAddress{"127.0.0.1", 8888}, poller);
        co_await client.Connect();
        co_await client.ReadSome(buf, size);
        co_return;
    }(loop.Poller(), rcv_buf, sizeof(rcv_buf));

    TFuture<void> h2 = [](TSocket* socket, char* buf, int size) -> TFuture<void>
    {
        TSocket clientSocket = std::move(co_await socket->Accept());
        auto s = co_await clientSocket.WriteSome(buf, size);
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
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char buf1[128] = {0};
    char buf2[128] = {0};

    TSocket client(TAddress{"127.0.0.1", 8888}, loop.Poller());

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
    TLoop loop;
    bool timeout = false;

    TFuture<void> h = [](TPoller& poller, bool& timeout) -> TFuture<void>
    {
        // TODO: use other addr
        TSocket client(TAddress{"10.0.0.1", 18889}, poller);
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
    }(loop.Poller(), timeout);

    while (!h.done()) {
        loop.Step();
    }

    assert_true(timeout);
}

template<typename TPoller>
void test_remove_connection_timeout(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", 18889}, loop.Poller());
    socket.Bind();
    socket.Listen();

    bool timeout = false;

    TFuture<void> h = [](TPoller& poller, bool& timeout) -> TFuture<void>
    {
        TSocket client(TAddress{"127.0.0.1", 18889}, poller);
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
    }(loop.Poller(), timeout);

    while (!h.done()) {
        loop.Step();
    }

    assert_false(timeout);
}

template<typename TPoller>
void test_connection_refused_on_write(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    std::error_code err;

    TFuture<void> h = [](TPoller& poller, std::error_code* err) -> TFuture<void>
    {
        TSocket clientSocket(TAddress{"127.0.0.1", 8888}, poller);
        char buffer[] = "test";
        try {
            co_await clientSocket.Connect();
            co_await clientSocket.WriteSome(buffer, sizeof(buffer));
        } catch (const std::system_error& ex) {
            *err = ex.code();
        }
        co_return;
    }(loop.Poller(), &err);

    while (!h.done()) {
        loop.Step();
    }

    // EPIPE in MacOS
    assert_true(err.value() == ECONNREFUSED || err.value() == EPIPE);
}

template<typename TPoller>
void test_connection_refused_on_read(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    std::error_code err;

    TFuture<void> h = [](TPoller& poller, std::error_code* err) -> TFuture<void>
    {
        TSocket clientSocket(TAddress{"127.0.0.1", 8888}, poller);
        char buffer[] = "test";
        try {
            co_await clientSocket.Connect();
            co_await clientSocket.ReadSome(buffer, sizeof(buffer));
        } catch (const std::system_error& ex) {
            *err = ex.code();
        }
        co_return;
    }(loop.Poller(), &err);

    while (!h.done()) {
        loop.Step();
    }

    assert_int_equal(err.value(), ECONNREFUSED);
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

    std::vector<char> data(1024*1024);
    int cur = 0;
    for (auto& ch : data) {
        ch = cur + 'a';
        cur = (cur + 1) % ('z' - 'a' + 1);
    }

    TLoop loop;
    TSocket socket(NNet::TAddress{"127.0.0.1", 8988}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TSocket client(NNet::TAddress{"127.0.0.1", 8988}, loop.Poller());

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

    assert_memory_equal(data.data(), received.data(), data.size());
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

    TLoop loop;
    TSocket socket(NNet::TAddress{"127.0.0.1", 8988}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TSocket client(NNet::TAddress{"127.0.0.1", 8988}, loop.Poller());

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

    TLoop loop;
    TSocket socket(NNet::TAddress{"127.0.0.1", 8988}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TFuture<void> h1 = [](auto& poller, auto& lines) -> TFuture<void>
    {
        TSocket client(NNet::TAddress{"127.0.0.1", 8988}, poller);
        co_await client.Connect();
        for (auto& line : lines) {
            co_await TByteWriter(client).Write(line.data(), line.size());
        }
        co_return;
    }(loop.Poller(), lines);

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

void test_self_id(void**) {
    void* id;
    TFuture<void> h = [](void** id) -> TFuture<void> {
        *id = (co_await SelfId()).address();
        co_return;
    }(&id);

    assert_ptr_equal(id, h.address());
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
    TResolver<TPollerBase> resolver(loop.Poller());

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

    std::vector<char> data(1024);
    int cur = 0;
    for (auto& ch : data) {
        ch = cur + 'a';
        cur = (cur + 1) % ('z' - 'a' + 1);
    }

    TLoop loop;
    TSocket socket(NNet::TAddress{"127.0.0.1", 8988}, loop.Poller());
    socket.Bind();
    socket.Listen();

    TSocket client(NNet::TAddress{"127.0.0.1", 8988}, loop.Poller());

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
    TVoidSuspendedTask h = [](TUring* uring, int* r) -> TVoidSuspendedTask {
        *r = uring->Result();
        co_return;
    }(&uring, &r);
    uring.Write(p[1], buf, 1, h);
    assert_true(!h.done());
    assert_int_equal(uring.Wait(), 1);
    uring.WakeupReadyHandles();
    int err = read(p[0], rbuf, 1);
    assert_true(rbuf[0] == 'e');
    assert_int_equal(r, 1);
    assert_true(h.done());
    h.destroy();
}

void test_uring_read_resume(void**) {
    TUring uring(256);
    char buf[1] = {'e'};
    char rbuf[1] = {'k'};
    int p[2];
    assert_int_equal(0, pipe(p));
    int r = 31337;
    TVoidSuspendedTask h = [](TUring* uring, int* r) -> TVoidSuspendedTask {
        *r = uring->Result();
        co_return;
    }(&uring, &r);
    assert_int_equal(1, write(p[1], buf, 1));
    uring.Read(p[0], rbuf, 1, h);
    assert_true(!h.done());
    assert_int_equal(uring.Wait(), 1);
    uring.WakeupReadyHandles();
    assert_true(rbuf[0] == 'e');
    assert_int_equal(r, 1);
    assert_true(h.done());
    h.destroy();
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
#else
#define my_unit_poller(f) my_unit_test2(f, TSelect, TPoll)
#endif

int main() {
    signal(SIGPIPE, SIG_IGN);
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
        my_unit_test2(test_read_write_full_ssl, TSelect, TPoll),
        my_unit_test2(test_resolver, TSelect, TPoll),
        my_unit_test2(test_resolve_bad_name, TSelect, TPoll),
#ifdef __linux__
        cmocka_unit_test(test_uring_create),
        cmocka_unit_test(test_uring_write),
        cmocka_unit_test(test_uring_read),
        cmocka_unit_test(test_uring_read_more_than_write),
        // cmocka_unit_test(test_uring_write_resume), // temporary disable
        // cmocka_unit_test(test_uring_read_resume), // temporary disable
        cmocka_unit_test(test_uring_no_sqe),
        // cmocka_unit_test(test_uring_cancel), // temporary disable
#endif
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
