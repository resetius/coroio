#include <exception>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>

#include <all.hpp>

extern "C" {
#include <cmocka.h>
}

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

void test_timeval(void**) {
    auto t1 =  std::chrono::seconds(4);
    auto t2 =  std::chrono::seconds(10);
    auto tv = GetTimeval(TTime(t1), TTime(t2));
    assert_int_equal(tv.tv_sec, 6);
    assert_int_equal(tv.tv_usec, 0);

    auto t3 =  std::chrono::milliseconds(10001);
    tv = GetTimeval(TTime(t1), TTime(t3));
    assert_int_equal(tv.tv_sec, 6);
    assert_int_equal(tv.tv_usec, 1000);

    auto t4 = std::chrono::minutes(10000);
    tv = GetTimeval(TTime(t1), TTime(t4));
    assert_int_equal(tv.tv_sec, 10);
    assert_int_equal(tv.tv_usec, 0);
}

void test_timespec(void**) {
    auto t1 =  std::chrono::seconds(4);
    auto t2 =  std::chrono::seconds(10);
    auto ts = GetTimespec(TTime(t1), TTime(t2));
    assert_int_equal(ts.tv_sec, 6);
    assert_int_equal(ts.tv_nsec, 0);

    auto t3 =  std::chrono::milliseconds(10001);
    ts = GetTimespec(TTime(t1), TTime(t3));
    assert_int_equal(ts.tv_sec, 6);
    assert_int_equal(ts.tv_nsec, 1000*1000);

    auto t4 = std::chrono::minutes(10000);
    ts = GetTimespec(TTime(t1), TTime(t4));
    assert_int_equal(ts.tv_sec, 10);
    assert_int_equal(ts.tv_nsec, 0);
}

void test_millis(void**) {
    auto t1 =  std::chrono::seconds(4);
    auto t2 =  std::chrono::seconds(10);
    auto m= GetMillis(TTime(t1), TTime(t2));
    assert_int_equal(m, 6000);

    auto t3 =  std::chrono::milliseconds(10001);
    m = GetMillis(TTime(t1), TTime(t3));
    assert_int_equal(m, 6001);

    auto t4 =  std::chrono::minutes(10000);
    m = GetMillis(TTime(t1), TTime(t4));
    assert_int_equal(m, 10000);
}

void test_addr(void**) {
    TAddress address("127.0.0.1", 8888);
    auto low = address.Addr();
    assert_true(low.sin_port == ntohs(8888));
    assert_true(low.sin_family == AF_INET);

    unsigned int value = ntohl((127<<24)|(0<<16)|(0<<8)|1);
    assert_true(memcmp(&low.sin_addr, &value, 4) == 0);
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

    TTestTask h1 = [](TPoller& poller) -> TTestTask
    {
        TSocket client(TAddress{"127.0.0.1", 8888}, poller);
        co_await client.Connect();
        co_return;
    }(loop.Poller());

    TTestTask h2 = [](TSocket* socket, TSocket* clientSocket) -> TTestTask
    {
        *clientSocket = std::move(co_await socket->Accept());
        co_return;
    }(&socket, &clientSocket);

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }
    h1.destroy(); h2.destroy();

    in_addr addr1 = clientSocket.Addr().Addr().sin_addr;
    in_addr addr2 = socket.Addr().Addr().sin_addr;
    assert_true(memcmp(&addr1, &addr2, 4)==0);
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

    TTestTask h1 = [](TPoller& poller, char* buf, int size) -> TTestTask
    {
        TSocket client(TAddress{"127.0.0.1", 8898}, poller);
        co_await client.Connect();
        co_await client.WriteSome(buf, size);
        co_return;
    }(loop.Poller(), send_buf, sizeof(send_buf));

    TTestTask h2 = [](TSocket* socket, char* buf, int size) -> TTestTask
    {
        TSocket clientSocket = std::move(co_await socket->Accept());
        co_await clientSocket.ReadSome(buf, size);
        co_return;
    }(&socket, rcv_buf, sizeof(rcv_buf));

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }
    h1.destroy(); h2.destroy();

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

    TTestTask h1 = [](TPoller& poller, char* buf, int size) -> TTestTask
    {
        TSocket client(TAddress{"127.0.0.1", 8888}, poller);
        co_await client.Connect();
        co_await client.ReadSome(buf, size);
        co_return;
    }(loop.Poller(), rcv_buf, sizeof(rcv_buf));

    TTestTask h2 = [](TSocket* socket, char* buf, int size) -> TTestTask
    {
        TSocket clientSocket = std::move(co_await socket->Accept());
        auto s = co_await clientSocket.WriteSome(buf, size);
        co_return;
    }(&socket, send_buf, sizeof(send_buf));

    while (!(h1.done() && h2.done())) {
        loop.Step();
    }
    h1.destroy(); h2.destroy();

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

    TTestTask h1 = [](TSocket& client) -> TTestTask
    {
        co_await client.Connect();
        co_return;
    }(client);

    TTestTask h2 = [](TSocket* socket, char* buf, int size) -> TTestTask
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

    TTestTask h3 = [](TSocket& client) -> TTestTask
    {
        char b[128] = "Hello from client";
        co_await client.WriteSomeYield(b, sizeof(b));
        co_return;
    }(client);

    TTestTask h4 = [](TSocket& client, char* buf, int size) -> TTestTask
    {
        co_await client.ReadSomeYield(buf, size);
        co_return;
    }(client, buf2, sizeof(buf2));

    while (!(h1.done() && h2.done() && h3.done() && h4.done())) {
        loop.Step();
    }

    assert_string_equal(buf1, "Hello from client");
    assert_string_equal(buf2, "Hello from server");

    h1.destroy();
    h2.destroy();
    h3.destroy();
    h4.destroy();
}

template<typename TPoller>
void test_connection_timeout(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    bool timeout = false;

    TTestTask h = [](TPoller& poller, bool& timeout) -> TTestTask
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
    h.destroy();

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

    TTestTask h = [](TPoller& poller, bool& timeout) -> TTestTask
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
    h.destroy();

    assert_false(timeout);
}

template<typename TPoller>
void test_connection_refused_on_write(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    std::error_code err;

    TTestTask h = [](TPoller& poller, std::error_code* err) -> TTestTask
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
    h.destroy();

    // EPIPE in MacOS
    assert_true(err.value() == ECONNREFUSED || err.value() == EPIPE);
}

template<typename TPoller>
void test_connection_refused_on_read(void**) {
    using TLoop = TLoop<TPoller>;
    using TSocket = typename TPoller::TSocket;
    TLoop loop;
    std::error_code err;

    TTestTask h = [](TPoller& poller, std::error_code* err) -> TTestTask
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
    h.destroy();

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
    TTestTask h = [](TPollerBase& poller, TTime* next, std::chrono::milliseconds timeout) -> TTestTask
    {
        co_await poller.Sleep(timeout);
        *next = std::chrono::steady_clock::now();
        co_return;
    } (loop.Poller(), &next, timeout);

    while (!h.done()) {
        loop.Step();
    }
    h.destroy();

    assert_true(next >= now + timeout);
}

#ifdef __linux__
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
    TTestSuspendTask h = []() -> TTestSuspendTask { co_return; }();
    uring.Read(p[0], rbuf, sizeof(rbuf), h);
    assert_int_equal(uring.Wait(), 1);
    assert_int_equal(uring.Result(), 1);
    assert_true(rbuf[0] == 'e');
    h.destroy();
}

void test_uring_write_resume(void**) {
    TUring uring(256);
    char buf[1] = {'e'};
    char rbuf[1] = {'k'};
    int p[2];
    assert_int_equal(0, pipe(p));
    int r = 31337;
    TTestSuspendTask h = [](TUring* uring, int* r) -> TTestSuspendTask {
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
    TTestSuspendTask h = [](TUring* uring, int* r) -> TTestSuspendTask {
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
        cmocka_unit_test(test_timeval),
        cmocka_unit_test(test_timespec),
        cmocka_unit_test(test_millis),
        my_unit_poller(test_listen),
        my_unit_poller(test_timeout),
        my_unit_poller(test_accept),
        my_unit_poller(test_write_after_connect),
        my_unit_poller(test_write_after_accept),
        my_unit_poller(test_connection_timeout),
        my_unit_poller(test_remove_connection_timeout),
        my_unit_poller(test_connection_refused_on_write),
        my_unit_poller(test_connection_refused_on_read),
        my_unit_poller(test_read_write_same_socket),
#ifdef __linux__
        cmocka_unit_test(test_uring_create),
        cmocka_unit_test(test_uring_write),
        cmocka_unit_test(test_uring_read),
        cmocka_unit_test(test_uring_read_more_than_write),
        cmocka_unit_test(test_uring_write_resume),
        cmocka_unit_test(test_uring_read_resume),
        cmocka_unit_test(test_uring_no_sqe),
        cmocka_unit_test(test_uring_cancel),
#endif
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
