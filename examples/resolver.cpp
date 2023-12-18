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
TVoidSuspendedTask run(TPoller& poller) {
    TFileHandle input{0, poller}; // stdin
    TLineReader lineReader(input, 4096);
    TResolver<TPollerBase> resolver(TAddress{"8.8.8.8", 53}, poller);
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

int main(int argc, char** argv) {
    TLoop<TDefaultPoller> loop;
    auto h = run(loop.Poller());
    while (!h.done()) {
        loop.Step();
    }
    return 0;
}
