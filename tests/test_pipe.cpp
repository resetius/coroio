#include <chrono>
#include <array>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>
#include <errno.h>
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

void test_pipe_merge_stderr_into_stdout(void**) {
    TLoop<TDefaultPoller> loop;
    auto& poller = loop.Poller();
    std::string bashPath = "/bin/bash";
    // Create pipe with stderr merged into stdout
    TPipe pipe(poller, bashPath, {"-c", "echo 'err to out' 1>&2"}, true);

    char buffer[64] = {};
    size_t bytesRead = sizeof(buffer);
    auto reader = [](TPipe& pipe, char* buffer, size_t& size) -> TFuture<void> {
        try {
            size = co_await pipe.ReadSome(buffer, size);
        } catch (const std::exception& ex) {
            std::cerr << "Read error: " << ex.what() << "\n";
        }
    }(pipe, buffer, bytesRead);

    while (!reader.done()) {
        loop.Step();
    }

    const char expected[] = "err to out\n";
    assert_true(bytesRead == sizeof(expected)-1);
    assert_true(memcmp(buffer, expected, sizeof(expected)-1) == 0);
}

void test_pipe_kill_then_read(void**) {
    TLoop<TDefaultPoller> loop;
    auto& poller = loop.Poller();
    std::string sleepPath = "/bin/sleep";
    // Start a process that produces no output and runs for a while
    TPipe pipe(poller, sleepPath, {"5"});

    int pid = pipe.Pid();
    assert_true(pid > 0);

    int rc = kill(pid, SIGKILL);
    assert_true(rc == 0 || errno == ESRCH);

    // Attempt to read from stdout; should observe EOF (0 bytes)
    char buffer[8] = {};
    size_t bytesRead = sizeof(buffer);
    auto reader = [](TPipe& pipe, char* buffer, size_t& size) -> TFuture<void> {
        try {
            size = co_await pipe.ReadSome(buffer, size);
        } catch (const std::exception& ex) {
            // Accept exceptions as valid outcome on abrupt termination
            size = 0;
        }
    }(pipe, buffer, bytesRead);

    while (!reader.done()) {
        loop.Step();
    }

    assert_true(bytesRead == 0);
}

void test_pipe_wait_clears_pid(void**) {
    TLoop<TDefaultPoller> loop;
    auto& poller = loop.Poller();
    std::string shPath = "/bin/sh";
    TPipe pipe(poller, shPath, {"-c", "exit 5"});

    assert_true(pipe.Pid() > 0);
    assert_int_equal(pipe.Wait(), 5);
    assert_int_equal(pipe.Pid(), -1);
    assert_int_equal(pipe.Wait(), -1);
}

template<typename TPoller>
void test_pipe_async_wait(void**) {
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    std::string shPath = "/bin/sh";
    // Closes stdout right away, exits later
    TPipe pipe(poller, shPath, {"-c", "exec >&-; sleep 0.3; exit 3"});

    int exitCode = -2;
    auto waiter = [](TPipe& pipe, int& exitCode) -> TFuture<void> {
        char buffer[64];
        while (co_await pipe.ReadSome(buffer, sizeof(buffer)) != 0) { }
        exitCode = co_await pipe.AsyncWait();
    }(pipe, exitCode);

    int ticks = 0;
    auto ticker = [](TPoller& poller, TFuture<void>& waiter, int& ticks) -> TFuture<void> {
        while (!waiter.done()) {
            co_await poller.Sleep(std::chrono::milliseconds(10));
            ticks++;
        }
    }(poller, waiter, ticks);

    while (!waiter.done() || !ticker.done()) {
        loop.Step();
    }

    assert_int_equal(exitCode, 3);
    assert_true(ticks > 5);
    assert_int_equal(pipe.Pid(), -1);
}

#define my_unit_test(f, a) { #f "(" #a ")", f<a>, NULL, NULL, NULL }

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
    ADD_TEST(cmocka_unit_test, test_pipe_merge_stderr_into_stdout);
    ADD_TEST(cmocka_unit_test, test_pipe_kill_then_read);
    ADD_TEST(cmocka_unit_test, test_pipe_wait_clears_pid);
    ADD_TEST(my_unit_test, test_pipe_async_wait, TSelect);
    ADD_TEST(my_unit_test, test_pipe_async_wait, TPoll);
#ifdef __linux__
    ADD_TEST(my_unit_test, test_pipe_async_wait, TEPoll);
#ifdef HAVE_URING
    ADD_TEST(my_unit_test, test_pipe_async_wait, TUring);
#endif
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
    ADD_TEST(my_unit_test, test_pipe_async_wait, TKqueue);
#endif
#endif

    return _cmocka_run_group_tests("test_pipe", tests.data(), tests.size(), NULL, NULL);
}
