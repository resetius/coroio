#include "net.hpp"
#include <signal.h>

using namespace NNet;

TSimpleTask client_handler(TSocket&& s, TLoop* loop ) {
    char buffer[1024] = {0};
    TSocket socket(std::move(s));

    while (true) {
        buffer[0] = '\0';
        auto size = co_await socket.ReadSome(buffer, sizeof(buffer));
        if (size <= 0) {
            std::cerr << "Connection closed " << size << "\n";
        }

        if (size > 0) {
            std::cerr << "Received from client: " << buffer << "\n";
        }

        co_await socket.WriteSome(buffer, sizeof(buffer));
        //co_await loop->Sleep(std::chrono::milliseconds(10));
    }

    std::cerr << "Return\n";

    co_return;
}

TSimpleTask server(TLoop* loop)
{
    TAddress address("127.0.0.1", 8888);
    TSocket socket(std::move(address), loop);
    socket.Bind();
    socket.Listen();
    int i = 1;

    while (true) {
        auto client = co_await socket.Accept();
        std::cerr << "Accepted " << i++ << "\n";
        client_handler(std::move(client), loop); // TODO: destroy coro on client disconnect
        // co_await loop->Sleep(std::chrono::milliseconds(1000));
    }
}

TSimpleTask client(TLoop* loop)
{
    TAddress address("127.0.0.1", 8888);
    char buffer[] = "Hello\n\0";
    char rcv[1024] = {0};

    TSocket socket(address, loop);
    co_await socket.Connect();
    std::cerr << "Connected\n";
    while (true) {
        auto size = co_await socket.WriteSome(buffer, sizeof(buffer));
        //std::cerr << "Bytes written : " << size << "\n";
        size = co_await socket.ReadSome(rcv, sizeof(rcv));
        std::cerr << "Received from server: " << rcv << std::endl;
        co_await loop->Sleep(std::chrono::milliseconds(1000));
    }
    std::cerr << "Return\n";
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    TLoop loop;

    server(&loop);
    client(&loop);

    loop.Loop();
}
