#include "net.hpp"

using namespace NNet;

struct promise;

struct task : std::coroutine_handle<promise>
{
    using promise_type = ::promise;
};

struct promise
{
    task get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};

task infinite_task(TLoop* loop) {
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
