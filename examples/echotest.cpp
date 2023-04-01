#include "net.hpp"
#include <signal.h>

using namespace NNet;

TSimpleTask client_handler(TSocket socket, TLoop* loop) {
    char buffer[128] = {0};

    while (true) {
        auto size = co_await socket.ReadSome(buffer, sizeof(buffer));
        std::cerr << "Received from client: " << buffer << " (" << size << ") bytes \n";

        co_await socket.WriteSome(buffer, sizeof(buffer));
    }
    co_return;
}

TSimpleTask server(TLoop* loop)
{
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop);
    socket.Bind();
    socket.Listen();

    while (true) {
        auto client = co_await socket.Accept();
        client_handler(std::move(client), loop);
    }
    co_return;
}

TSimpleTask client(TLoop* loop, int clientId)
{
    TAddress address("127.0.0.1", 8888);
    char buffer[128] = "Hello XXX/YYY";
    char rcv[1024] = {0};
    int messageNo = 1;

    TSocket socket(address, loop);
    co_await socket.Connect();

    while (true) {
        snprintf(buffer+6, sizeof(buffer)-6, "%03d/%03d", messageNo++, clientId);
        auto size = co_await socket.WriteSome(buffer, sizeof(buffer));
        size = co_await socket.ReadSome(rcv, sizeof(rcv));
        std::cerr << "Received from server: " << rcv << " (" << size << ") bytes \n";
        co_await loop->Sleep(std::chrono::milliseconds(1000));
    }
    co_return;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    TLoop loop;
    int clients = 0;
    if (argc > 1) { clients = atoi(argv[1]); }
    if (clients == 0) { clients = 1; }

    server(&loop);
    for (int i = 0; i < clients; i++) {
        client(&loop, i+1);
    }

    loop.Loop();
}
