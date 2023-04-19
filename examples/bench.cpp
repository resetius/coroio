#include <chrono>
#include <coroutine>
#include <exception>
#include <algorithm>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <net.hpp>
#include <poll.hpp>
#include <select.hpp>
#include <system_error>

#ifdef __linux__
#include <epoll.hpp>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <kqueue.hpp>
#endif

using namespace NNet;
using namespace std;

namespace {

void usage(const char* name) {
    printf("%s [-n num_pipes] [-a num_active] [-w num_writes] [-m method]\n", name);
}

struct Stat {
    int writes = 0;
    int fired = 0;
    int count = 0;
    int failures = 0;
    int out = 0;
};

TTestTask pipe_reader(TSocket& r, TSocket& w, Stat& s) {
    ssize_t size;
    char buf[1] = {0};

    try {
        while ((size = co_await r.ReadSomeYield(buf, 1)) != 0) {
            s.count += size;
            if (s.writes) {
                if (co_await w.WriteSome(buf, 1) != 1) {
                    s.failures ++;
                }
                s.writes--;
                s.fired++;
            }
        }
    } catch (const std::exception& ) {
        s.failures ++;
    }
    s.out ++;
    co_return;
}

TTestTask write_one(TSocket& w, Stat& s) {
    char buf[1] = {'e'};
    co_await w.WriteSome(buf, 1);
    s.fired ++;
    co_return;
}

TTestTask yield(TPollerBase& poller) {
    co_await poller.Sleep(std::chrono::milliseconds(0));
    co_return;
}

template<typename TPoller>
std::chrono::microseconds run_one(int num_pipes, int num_writes, int num_active) {
    Stat s;
    TLoop<TPoller> loop;
    vector<TSocket> pipes;
    vector<coroutine_handle<>> handles;
    pipes.reserve(num_pipes*2);
    handles.reserve(num_pipes+num_writes);
    int fired = 0;
    for (int i = 0; i < num_pipes; i++) {
        int p[2];
        if (pipe(&p[0]) < 0) {
            throw std::system_error(errno, std::generic_category(), "pipe");
        }
        pipes.emplace_back(std::move(TSocket{{}, p[0], loop.Poller()}));
        pipes.emplace_back(std::move(TSocket{{}, p[1], loop.Poller()}));
    }

    s.writes = num_writes;

    for (int i = 0; i < num_pipes; i++) {
        handles.emplace_back(pipe_reader(pipes[i*2], pipes[((i+1)%num_pipes)*2+1], s));
    }

    handles.emplace_back(yield(loop.Poller())); // initialize events (sleep in readers)
    loop.Step();

    int space = num_pipes/num_active;
    space *= 2;
    for (int i = 0; i < num_active; i++) {
        handles.emplace_back(write_one(pipes[i*space+1], s));
    }

    auto t1 = TClock::now();
    int xcount = 0;
    do {
        loop.Step();
        xcount ++;
    } while (s.fired != s.count);
    auto t2 = TClock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(t2-t1);
    cerr << "fired: " << s.fired << ", "
         << "writes: " << s.writes << ", "
         << "count: " << s.count << ", "
         << "xcount: " << xcount << ", "
         << "failures: " << s.failures << ", "
         << "out: " << s.out << endl;
    cerr << "elapsed: " <<  duration.count() << endl;

    for (auto& h : handles) {
        h.destroy();
    }

    return duration;
}

template<typename TPoller>
void run_test(int num_pipes, int num_writes, int num_active) {
    int runs = 25;
    vector<uint64_t> results;
    results.reserve(runs);
    for (int i = 0; i < runs; i++) {
        auto d = run_one<TPoller>(num_pipes, num_writes, num_active).count();
        results.emplace_back(d);
    }
    sort(results.begin(), results.end());
    cout << "min: " << results[0] << endl;
    cout << "max: " << results[runs-1] << endl;
    cout << "p50: " << results[0.5*runs] << endl;
    cout << "p90: " << results[0.9*runs] << endl;
    cout << "p95: " << results[0.95*runs] << endl;
    cout << "p99: " << results[0.99*runs] << endl;
}

} // namespace {

int main(int argc, char** argv) {
    int num_pipes = 100;
    int num_writes = num_pipes;
    int num_active = 1;
    const char* method = "poll";

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-n") && i < argc-1) {
            num_pipes = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-a") && i < argc-1) {
            num_active = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-w") && i < argc-1) {
            num_writes = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-m") && i < argc-1) {
            method = argv[++i];
        } else {
            usage(argv[0]); return 1;
        }
    }
    if (!strcmp(method, "poll")) {
        run_test<TPoll>(num_pipes, num_writes, num_active);
    } else if (!strcmp(method, "select")) {
        run_test<TSelect>(num_pipes, num_writes, num_active);
    }
#ifdef __linux__
    else if (!strcmp(method, "epoll")) {
        run_test<TEPoll>(num_pipes, num_writes, num_active);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
    else if (!strcmp(method, "kqueue")) {
        run_test<TKqueue>(num_pipes, num_writes, num_active);
    }
#endif

    else {
        std::cerr << "Unknown method: " << method << "\n";
    }
    return 0;
}
