#include "net.hpp"
#include <signal.h>

using namespace NNet;

TSimpleTask client_handler(TSocket&& s, TLoop* loop ) {
    char buffer[128] = {0};
    TSocket socket(std::move(s));

    while (true) {
        buffer[0] = '\0';
        auto size = co_await socket.ReadSome(buffer, sizeof(buffer));
        buffer[size] = 0;
        std::cerr << "Received from client: " << buffer << "\n";

        co_await socket.WriteSome(buffer, sizeof(buffer));
    }

    co_return;
}

TSimpleTask server(TLoop* loop)
{
    TAddress address("127.0.0.1", 8888);
    TSocket socket(std::move(address), loop);
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
        auto size = co_await socket.WriteSome(buffer, strlen(buffer));
        size = co_await socket.ReadSome(rcv, sizeof(rcv));
        std::cerr << "Received from server: " << rcv << std::endl;
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
