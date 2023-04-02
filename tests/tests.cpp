#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>

#include <net.hpp>

extern "C" {
#include <cmocka.h>
}

using namespace NNet;

void test_addr(void**) {
    TAddress address("127.0.0.1", 8888);
    auto low = address.Addr();
    assert_true(low.sin_port == ntohs(8888));
    assert_true(low.sin_family == AF_INET);

    unsigned int value = ntohl((127<<24)|(0<<16)|(0<<8)|1);
    assert_true(memcmp(&low.sin_addr, &value, 4) == 0);
}

void test_listen(void**) {
    TLoop loop;
    TAddress address("127.0.0.1", 8888);
    TSocket socket(std::move(address), loop.Poller());
    socket.Bind();
    socket.Listen();
}

void test_accept(void**) {
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop.Poller());
    TSocket clientSocket{};
    socket.Bind();
    socket.Listen();

    TSimpleTask h1 = [](TLoop* loop) -> TSimpleTask
    {
        TSocket client(TAddress{"127.0.0.1", 8888}, loop->Poller());
        co_await client.Connect();
        co_return;
    }(&loop);

    TSimpleTask h2 = [](TSocket* socket, TSocket* clientSocket) -> TSimpleTask
    {
        *clientSocket = std::move(co_await socket->Accept());
        co_return;
    }(&socket, &clientSocket);

    loop.OneStep();
    loop.HandleEvents();

    in_addr addr1 = clientSocket.Addr().Addr().sin_addr;
    in_addr addr2 = socket.Addr().Addr().sin_addr;
    assert_true(memcmp(&addr1, &addr2, 4)==0);
}

void test_connection_timeout(void**) {
    TLoop loop;
    TSocket socket(TAddress{"127.0.0.1", 8889}, loop.Poller());
    bool timeout = false;
    socket.Bind();
    socket.Listen();

    TSimpleTask h1 = [](TLoop& loop, bool& timeout) -> TSimpleTask
    {
        TSocket client(TAddress{"127.0.0.1", 8889}, loop.Poller());
        try {
            co_await client.Connect(TClock::now()+std::chrono::milliseconds(100));
        } catch (const TTimeout& ) {
            timeout = true;
        }
        co_return;
    }(loop, timeout);

    loop.OneStep();
    loop.HandleEvents();

    assert_true(timeout);
}

void test_connection_refused_on_write(void**) {
    TLoop loop;
    int err = 0;

    TSimpleTask h1 = [](TLoop* loop, int* err) -> TSimpleTask
    {
        TSocket clientSocket(TAddress{"127.0.0.1", 8888}, loop->Poller());
        char buffer[] = "test";
        try {
            co_await clientSocket.Connect();
            co_await clientSocket.WriteSome(buffer, sizeof(buffer));
        } catch (const TSystemError& ex) {
            *err = ex.Errno();
        }
        co_return;
    }(&loop, &err);

    loop.OneStep();
    loop.HandleEvents();

    assert_int_equal(err, ECONNREFUSED);
}

void test_connection_refused_on_read(void**) {
    TLoop loop;
    int err = 0;

    TSimpleTask h2 = [](TLoop* loop, int* err) -> TSimpleTask
    {
        TSocket clientSocket(TAddress{"127.0.0.1", 8888}, loop->Poller());
        char buffer[] = "test";
        try {
            co_await clientSocket.Connect();
            co_await clientSocket.ReadSome(buffer, sizeof(buffer));
        } catch (const TSystemError& ex) {
            *err = ex.Errno();
        }
        co_return;
    }(&loop, &err);

    loop.OneStep();
    loop.HandleEvents();

    assert_int_equal(err, ECONNREFUSED);
}

void test_timeout(void**) {
    TLoop loop;
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(100);
    TTime next;
    TSimpleTask h = [](TSelect& poller, TTime* next, std::chrono::milliseconds timeout) -> TSimpleTask
    {
        co_await poller.Sleep(timeout);
        *next = std::chrono::steady_clock::now();
        co_return;
    } (loop.Poller(), &next, timeout);
    loop.OneStep();
    loop.HandleEvents();
    assert_true(next >= now + timeout);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_addr),
        cmocka_unit_test(test_listen),
        cmocka_unit_test(test_timeout),
        cmocka_unit_test(test_accept),
        cmocka_unit_test(test_connection_timeout),
        cmocka_unit_test(test_connection_refused_on_write),
        cmocka_unit_test(test_connection_refused_on_read),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
