#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

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
    TSocket socket(std::move(address), &loop);
    socket.Bind();
    socket.Listen();
}

void test_timeout(void**) {
    TLoop loop;
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(100);
    TEvent::TTime next;
    TSimpleTask h = [](TLoop* loop, TEvent::TTime* next, std::chrono::milliseconds timeout) -> TSimpleTask 
    {
        co_await loop->Sleep(timeout);
        *next = std::chrono::steady_clock::now();
        co_return;
    } (&loop, &next, timeout);
    loop.OneStep();
    loop.HandleEvents();
    assert_true(next >= now + timeout);
}

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_addr),
        cmocka_unit_test(test_listen),
        cmocka_unit_test(test_timeout)
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
