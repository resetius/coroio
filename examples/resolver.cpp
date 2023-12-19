#include <coroio/all.hpp>

using namespace NNet;

template<typename TResolver>
TVoidTask resolve(TResolver& resolver, std::string name, int* inflight) {
    auto addrs = co_await resolver.Resolve(name);
    std::cout << "'" << name << "': ";
    for (auto& a : addrs) {
        std::cout << a.ToString() << ", ";
    }
    std::cout << "\n";
    --(*inflight);
    co_return;
}

template<typename TPoller>
TVoidSuspendedTask resolve(TPoller& poller) {
    TFileHandle input{0, poller}; // stdin
    TLineReader lineReader(input, 4096);
    TResolver<TPollerBase> resolver(poller);
    int inflight = 0;
    while (auto line = co_await lineReader.Read()) {
        inflight++;
        std::string name = std::string(line.Part1);
        name += line.Part2;
        name.resize(name.size()-1);
        resolve(resolver, std::move(name), &inflight);
    }
    while (inflight != 0) {
        co_await poller.Yield();
    }
    co_return;
}

template<typename TPoller>
void run() {
    TLoop<TPoller> loop;
    auto h = resolve(loop.Poller());
    while (!h.done()) {
        loop.Step();
    }
    h.destroy();
}

int main(int argc, char** argv) {
    std::string method = "poll";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--method") && i < argc-1) {
            method = argv[++i];
        }
    }

    if (method == "select") {
        run<TSelect>();
    } else if (method == "poll") {
        run<TPoll>();
    }
#ifdef __linux__
    else if (method == "epoll") {
        run<TEPoll>();
    }
    else if (method == "uring") {
        run<TUring>();
    }
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
    else if (method == "kqueue") {
        run<TKqueue>();
    }
#endif
    else {
        std::cerr << "Unknown method\n";
    }

    return 0;
}

