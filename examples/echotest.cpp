#include "net.hpp"
#include <signal.h>

using namespace NNet;

TSimpleTask client_handler(TSocket socket, TLoop* loop) {
    char buffer[128] = {0}; ssize_t size = 0;

    try {
        while ((size = co_await socket.ReadSome(buffer, sizeof(buffer))) > 0) {
            std::cerr << "Received from client: '" << std::string_view(buffer, size) << "' (" << size << ") bytes \n";
            co_await socket.WriteSome(buffer, size);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    if (size == 0) { 
        std::cerr << "Client disconnected\n";
    }
    co_return;
}

TSimpleTask server(TLoop* loop)
{
    TSocket socket(TAddress{"127.0.0.1", 8888}, loop);
    socket.Bind();
    socket.Listen();

    try {
        while (true) {
            auto client = co_await socket.Accept();
            client_handler(std::move(client), loop);
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

TSimpleTask client(TLoop* loop, int clientId)
{
    char buffer[128] = "Hello XXX/YYY";
    char rcv[128] = {0};
    int messageNo = 1;
    ssize_t size = 0;

    try {
        TSocket socket(TAddress{"127.0.0.1", 8888}, loop);
        co_await socket.Connect();

        do {
            snprintf(buffer+6, sizeof(buffer)-6, "%03d/%03d", messageNo++, clientId);
            size = co_await socket.WriteSome(buffer, sizeof(buffer));
            size = co_await socket.ReadSome(rcv, sizeof(rcv));
            std::cerr << "Received from server: " << std::string_view(rcv, size) << " (" << size << ") bytes \n";
            co_await loop->Sleep(std::chrono::milliseconds(1000));
        } while (size > 0);
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
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
    return 0;
}
