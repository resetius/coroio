#include "base.hpp"
#include "poller.hpp"
#include <chrono>
#include <exception>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <net.hpp>
#include <poll.hpp>
#include <select.hpp>

#ifdef __linux__
#include <epoll.hpp>
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

TSimpleTask pipe_reader(TSocket& r, TSocket& w, Stat& s) {
    ssize_t size;
    char buf[1] = {0};

    try {
        while ((size = co_await r.ReadSome(buf, 1)) != 0) {
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

TSimpleTask write_one(TSocket& w, Stat& s) {
    char buf[1] = {'e'};
    co_await w.WriteSome(buf, 1);
    s.fired ++;
    co_return;
}

template<typename TPoller>
void run_test(int num_pipes, int num_writes, int num_active) {
    Stat s;
    TLoop<TPoller> loop;
    vector<TSocket> pipes;
    pipes.reserve(num_pipes*2);
    int fired = 0;
    for (int i = 0; i < num_pipes; i++) {
        int p[2];
        if (pipe(&p[0]) < 0) { throw TSystemError(); }
        pipes.emplace_back(std::move(TSocket{{}, p[0], loop.Poller()}));
        pipes.emplace_back(std::move(TSocket{{}, p[1], loop.Poller()}));
    }

    s.writes = num_writes;

    for (int i = 0; i < num_pipes; i++) {
        pipe_reader(pipes[i*2], pipes[i*2+1], s);
    }

    for (int i = 0; i < num_active; i++) {
        write_one(pipes[i*2+1], s);
    }

    auto t1 = TClock::now();
    do {
        loop.Step();
    } while (s.fired != s.count);
    auto t2 = TClock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(t2-t1);
    printf("fired: %d, writes: %d, count: %d, failures: %d, out: %d\n", s.fired, s.writes, s.count, s.failures, s.out);
    printf("elapsed: %ld\n", duration.count());

    // TODO: free mem
    // TODO: multiple runs
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
    else {
        std::cerr << "Unknown method: " << method << "\n";
    }
    return 0;
}
