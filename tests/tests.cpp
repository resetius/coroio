#include <exception>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>

#include <net.hpp>
#include <select.hpp>
#include <poll.hpp>

extern "C" {
#include <cmocka.h>
}

using namespace NNet;

struct TTestPromise;

struct TTestTask : std::coroutine_handle<TTestPromise>
{
    using promise_type = TTestPromise;
};

struct TTestPromise
{
    TTestTask get_return_object() { return { TTestTask::from_promise(*this) }; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

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
    TSocket socket(TAddress{"127.0.0.1", 8889}, loop.Poller());
    bool timeout = false;
    socket.Bind();
    socket.Listen();

    TTestTask h = [](TPollerBase& poller, bool& timeout) -> TTestTask
    {
        TSocket client(TAddress{"127.0.0.1", 8889}, poller);
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

int main() {
    signal(SIGPIPE, SIG_IGN);
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_addr),
        cmocka_unit_test(test_listen<TSelect>),
        cmocka_unit_test(test_listen<TPoll>),
        cmocka_unit_test(test_timeout<TSelect>),
        cmocka_unit_test(test_timeout<TPoll>),
        cmocka_unit_test(test_accept<TSelect>),
        cmocka_unit_test(test_accept<TPoll>),
        cmocka_unit_test(test_write_after_connect<TSelect>),
        cmocka_unit_test(test_write_after_connect<TPoll>),
        cmocka_unit_test(test_write_after_accept<TSelect>),
        cmocka_unit_test(test_write_after_accept<TPoll>),
        cmocka_unit_test(test_connection_timeout<TSelect>),
        cmocka_unit_test(test_connection_timeout<TPoll>),
        cmocka_unit_test(test_connection_refused_on_write<TSelect>),
        cmocka_unit_test(test_connection_refused_on_write<TPoll>),
        cmocka_unit_test(test_connection_refused_on_read<TSelect>),
        cmocka_unit_test(test_connection_refused_on_read<TPoll>),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
