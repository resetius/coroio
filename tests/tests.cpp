#include <exception>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>

#include <net.hpp>
#include <select.hpp>
#include <poll.hpp>

#ifdef __linux__
#include <epoll.hpp>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <kqueue.hpp>
#endif

extern "C" {
#include <cmocka.h>
}

using namespace NNet;

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
    TLoop<TPoller> loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    TSocket clientSocket{};
    socket.Bind();
    socket.Listen();

    TTestTask h1 = [](TPollerBase& poller) -> TTestTask
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
    TLoop<TPoller> loop;
    TSocket socket(TAddress{"127.0.0.1", 8898}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[128] = {0};

    TTestTask h1 = [](TPollerBase& poller, char* buf, int size) -> TTestTask
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
    TLoop<TPoller> loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    socket.Bind();
    socket.Listen();
    char send_buf[128] = "Hello";
    char rcv_buf[128] = {0};

    TTestTask h1 = [](TPollerBase& poller, char* buf, int size) -> TTestTask
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
void test_connection_timeout(void**) {
    TLoop<TPoller> loop;
    bool timeout = false;

    TTestTask h = [](TPollerBase& poller, bool& timeout) -> TTestTask
    {
        // TODO: use other addr
        TSocket client(TAddress{"10.0.0.1", 18889}, poller);
        try {
            co_await client.Connect(TClock::now()+std::chrono::milliseconds(100));
        } catch (const TTimeout& ) {
            timeout = true;
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
void test_connection_refused_on_write(void**) {
    TLoop<TPoller> loop;
    int err = 0;

    TTestTask h = [](TPollerBase& poller, int* err) -> TTestTask
    {
        TSocket clientSocket(TAddress{"127.0.0.1", 8888}, poller);
        char buffer[] = "test";
        try {
            co_await clientSocket.Connect();
            co_await clientSocket.WriteSome(buffer, sizeof(buffer));
        } catch (const TSystemError& ex) {
            *err = ex.Errno();
        }
        co_return;
    }(loop.Poller(), &err);

    while (!h.done()) {
        loop.Step();
    }
    h.destroy();

    // EPIPE in MacOS
    assert_true(err == ECONNREFUSED || err == EPIPE);
}

template<typename TPoller>
void test_connection_refused_on_read(void**) {
    TLoop<TPoller> loop;
    int err = 0;

    TTestTask h = [](TPollerBase& poller, int* err) -> TTestTask
    {
        TSocket clientSocket(TAddress{"127.0.0.1", 8888}, poller);
        char buffer[] = "test";
        try {
            co_await clientSocket.Connect();
            co_await clientSocket.ReadSome(buffer, sizeof(buffer));
        } catch (const TSystemError& ex) {
            *err = ex.Errno();
        }
        co_return;
    }(loop.Poller(), &err);

    while (!h.done()) {
        loop.Step();
    }
    h.destroy();

    assert_int_equal(err, ECONNREFUSED);
}

template<typename TPoller>
void test_timeout(void**) {
    TLoop<TPoller> loop;
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

#define my_unit_test(f, a) { #f "(" #a ")", f<a>, NULL, NULL, NULL }
#define my_unit_test2(f, a, b) \
    { #f "(" #a ")", f<a>, NULL, NULL, NULL }, \
    { #f "(" #b ")", f<b>, NULL, NULL, NULL }
#define my_unit_test3(f, a, b, c) \
    { #f "(" #a ")", f<a>, NULL, NULL, NULL }, \
    { #f "(" #b ")", f<b>, NULL, NULL, NULL }, \
    { #f "(" #c ")", f<c>, NULL, NULL, NULL }

#ifdef __linux__
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TEPoll)
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TKqueue)
//#define my_unit_poller(f) my_unit_test(f, TSelect)
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
        my_unit_poller(test_connection_refused_on_write),
        my_unit_poller(test_connection_refused_on_read),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
