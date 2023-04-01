#include "net.hpp"

using namespace NNet;

TSimpleTask infinite_task(TLoop* loop) {
    int i = 0;
    while (true) {
        co_await loop->Sleep(std::chrono::milliseconds(10));
        printf("Ok %d\n", i++);
    }
}

int main() {
    TLoop loop;
    infinite_task(&loop);

    loop.Loop();
}
