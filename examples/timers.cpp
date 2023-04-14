#include "net.hpp"

using NNet::TSimpleTask;
using NNet::TSelect;
using TLoop = NNet::TLoop<TSelect>;

TSimpleTask infinite_task(TLoop* loop) {
    int i = 0;
    while (true) {
        co_await loop->Poller().Sleep(std::chrono::milliseconds(10));
        printf("Ok %d\n", i++);
    }
}

int main() {
    TLoop loop;
    infinite_task(&loop);

    loop.Loop();
}
