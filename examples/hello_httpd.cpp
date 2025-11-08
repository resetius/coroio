#include <chrono>
#include <iostream>
#include <vector>
#include <set>
#include <coroio/all.hpp>
#include <coroio/http/httpd.hpp>

using namespace NNet;

int main(int argc, char** argv) {
    NNet::TInitializer init;
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i < argc-1) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {
            std::cout << "Usage: " << argv[0] << " [--port port]\n";
            return 0;
        }
    }

    TAddress address{"::", port};
    std::cerr << "Starting HTTP server on port " << port << "\n";

    auto logger = [](const std::string& msg) {
        std::cout << "[HTTPD] " << msg << std::endl;
    };

    using TPoller = TDefaultPoller;
    using TSocket = typename TPoller::TSocket;
    TLoop<TPoller> loop;
    TSocket listenSocket(loop.Poller(), address.Domain());
    listenSocket.Bind(address);
    listenSocket.Listen();
    std::cerr << "Listening on: " << listenSocket.LocalAddr()->ToString() << std::endl;

    THelloWorldRouter router;
    TWebServer<TSocket> server(std::move(listenSocket), router, logger);
    server.Start();

    loop.Loop();
    return 0;
}
