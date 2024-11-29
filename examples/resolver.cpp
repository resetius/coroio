#include "coroio/resolver.hpp"
#include <coroio/all.hpp>

using namespace NNet;

template<typename TResolver>
TVoidTask resolve(TResolver& resolver, std::string name, EDNSType type, int* inflight) {
    try {
        auto addrs = co_await resolver.Resolve(name, type);
        std::cout << "'" << name << "': ";
        for (auto& a : addrs) {
            std::cout << a.ToString() << ", ";
        }
    } catch (const std::exception& ex) {
        std::cout << "'" << name << "': ";
        std::cout << ex.what();
    }
    std::cout << "\n";
    --(*inflight);
    co_return;
}

template<typename TPoller>
TFuture<void> resolve(TPoller& poller, EDNSType type) {
    TFileHandle input{0, poller}; // stdin
    TLineReader lineReader(input, 4096);
    TResolver<TPollerBase> resolver(poller);
    int inflight = 0;
    while (auto line = co_await lineReader.Read()) {
        inflight++;
        std::string name = std::string(line.Part1);
        name += line.Part2;
        name.resize(name.size()-1);
        resolve(resolver, std::move(name), type, &inflight);
    }
    while (inflight != 0) {
        co_await poller.Yield();
    }
    co_return;
}

template<typename TPoller>
void run(EDNSType type) {
    TLoop<TPoller> loop;
    auto h = resolve(loop.Poller(), type);
    while (!h.done()) {
        loop.Step();
    }
}

void usage(const char* name) {
    std::cerr << name << " [--method select|poll|epoll|kqueue] [--ipv6] [--help] < addr_file.txt" << std::endl;
    std::exit(1);
}

int main(int argc, char** argv) {
    std::string method = "poll";
    EDNSType type = EDNSType::A;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--method") && i < argc-1) {
            method = argv[++i];
        } else if (!strcmp(argv[i], "--ipv6")) {
            type = EDNSType::AAAA;
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[i]);
        }
    }

    if (method == "select") {
        run<TSelect>(type);
    }
    else if (method == "poll") {
        run<TPoll>(type);
    }
#ifdef HAVE_EPOLL
    else if (method == "epoll") {
        run<TEPoll>(type);
    }
#endif
#ifdef HAVE_URING
    else if (method == "uring") {
        run<TUring>(type);
    }
#endif
#ifdef HAVE_KQUEUE
    else if (method == "kqueue") {
        run<TKqueue>(type);
    }
#endif
    else {
        std::cerr << "Unknown method\n";
    }

    return 0;
}

