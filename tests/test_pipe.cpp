#include <chrono>
#include <array>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>
#include <iostream>
#include <unordered_set>

#include <coroio/all.hpp>
#include <coroio/pipe/pipe.hpp>

#include "testlib.h"
#include "perf.h"

extern "C" {
#include <cmocka.h>
}

using namespace NNet;

#ifndef _WIN32
void test_pipe_basic_read_write(void**) {
    TLoop<TDefaultPoller> loop;
    auto& poller = loop.Poller();
    std::string catPath = "/bin/cat";
    TPipe pipe(poller, catPath, {});
    const char testData[] = "Hello, Pipe!\n";

    auto writer = [](TPipe& pipe, const char* data, size_t size) -> TFuture<void>{
        try {
            size = co_await pipe.WriteSome(data, size);
        } catch (const std::exception& ex) {
            std::cerr << "Write error: " << ex.what() << "\n";
            co_return;
        }
    }(pipe, testData, sizeof(testData));

    char buffer[64] = {};
    size_t bytesRead = sizeof(buffer);
    auto reader = [](TPipe& pipe, char* buffer, size_t& size) -> TFuture<void> {
        try {
            size = co_await pipe.ReadSome(buffer, size);
        } catch (const std::exception& ex) {
            std::cerr << "Read error: " << ex.what() << "\n";
        }
    }(pipe, buffer, bytesRead);

    while (!writer.done() || !reader.done()) {
        loop.Step();
    }

    assert_true(bytesRead == sizeof(testData));
    assert_true(memcmp(buffer, testData, sizeof(testData)) == 0);
}

void test_pipe_read_stderr(void**) {
    TLoop<TDefaultPoller> loop;
    auto& poller = loop.Poller();
    std::string bashPath = "/bin/bash";
    TPipe pipe(poller, bashPath, {"-c", "echo 'error message' 1>&2"});

    char buffer[64] = {};
    size_t bytesRead = sizeof(buffer);
    auto reader = [](TPipe& pipe, char* buffer, size_t& size) -> TFuture<void> {
        try {
            size = co_await pipe.ReadSomeErr(buffer, size);
        } catch (const std::exception& ex) {
            std::cerr << "Read error: " << ex.what() << "\n";
        }
    }(pipe, buffer, bytesRead);

    while (!reader.done()) {
        loop.Step();
    }

    const char expected[] = "error message\n";
    assert_true(bytesRead == sizeof(expected)-1);
    assert_true(memcmp(buffer, expected, sizeof(expected)-1) == 0);
}

#endif // _WIN32

int main(int argc, char** argv) {
    TInitializer init;

    std::vector<CMUnitTest> tests;
    std::unordered_set<std::string> filters;
    tests.reserve(500);

    parse_filters(argc, argv, filters);

#ifndef _WIN32
    ADD_TEST(cmocka_unit_test, test_pipe_basic_read_write);
    ADD_TEST(cmocka_unit_test, test_pipe_read_stderr);
#endif

    return _cmocka_run_group_tests("test_pipe", tests.data(), tests.size(), NULL, NULL);
}
