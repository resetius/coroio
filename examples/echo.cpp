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

task client_handler(TSocket&& socket) {
    char buffer[1024];

    while (true) {
        auto size = co_await socket.Read(buffer, sizeof(buffer));
        if (size <= 0) {
            std::cerr << "Connection closed\n";
        }

        co_await socket.Write(buffer, sizeof(buffer));
    }
}

task server(TLoop* loop)
{
    TAddress address("127.0.0.1", 8888);
    TSocket socket(std::move(address), loop);
    socket.Bind();
    socket.Listen();

    while (true) {
        auto client = co_await socket.Accept();
        client_handler(std::move(client));
    }
}

task client(TLoop* loop)
{
    TAddress address("127.0.0.1", 8888);
    char buffer[] = "Hello";
    char rcv[1024] = {0};

    TSocket socket(address, loop);
    co_await socket.Connect();
    std::cerr << "Connected\n";

    while (true) {
        co_await socket.Write(buffer, sizeof(buffer));
        auto size = co_await socket.Read(rcv, sizeof(rcv));
        std::cerr << "Received: " << rcv << std::endl;
        co_await loop->Sleep(std::chrono::milliseconds(1000));
    }
}

int main() {
    TLoop loop;

    //server(&loop);
    client(&loop);

    loop.Loop();
}
