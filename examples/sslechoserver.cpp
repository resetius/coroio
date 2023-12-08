#include <coroio/all.hpp>

using NNet::TVoidTask;
using NNet::TAddress;
using NNet::TSelect;
using NNet::TPoll;

#ifdef __linux__
using NNet::TEPoll;
using NNet::TUring;
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
using NNet::TKqueue;
#endif

template<bool debug, typename TSocket>
TVoidTask client_handler(TSocket socket, int buffer_size) {
    std::vector<char> buffer(buffer_size); ssize_t size = 0;

    try {
        NNet::TSslContext ctx = NNet::TSslContext::Server("server.crt", "server.key");
        NNet::TSslSocket<TSocket> sslSocket(socket, ctx, [&](const char* s) { std::cerr << s << "\n"; });

        co_await sslSocket.Accept();
        while ((size = co_await sslSocket.ReadSome(buffer.data(), buffer_size)) > 0) {
            if constexpr(debug) {
                std::cerr << "Received: " << std::string_view(buffer.data(), size) << "\n";
            }
            co_await sslSocket.WriteSome(buffer.data(), size);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    if (size == 0) {
        std::cerr << "Client disconnected\n";
    }
    co_return;
}

template<bool debug, typename TPoller>
TVoidTask server(TPoller& poller, TAddress address, int buffer_size)
{
    typename TPoller::TSocket socket(std::move(address), poller);
    socket.Bind();
    socket.Listen();

    while (true) {
        auto client = co_await socket.Accept();
        if (debug) {
            std::cerr << "Accepted\n";
        }
        client_handler<debug>(std::move(client), buffer_size);
    }
    co_return;
}

template<typename TPoller>
void run(bool debug, TAddress address, int buffer_size)
{
    NNet::TLoop<TPoller> loop;
    if (debug) {
        server<true>(loop.Poller(), std::move(address), buffer_size);
    } else {
        server<false>(loop.Poller(), std::move(address), buffer_size);
    }
    loop.Loop();
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    int port = 0;
    int buffer_size = 128;
    std::string method = "select";
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i < argc-1) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--method") && i < argc-1) {
            method = argv[++i];
        } else if (!strcmp(argv[i], "--debug")) {
            debug = true;
        } else if (!strcmp(argv[i], "--buffer-size") && i < argc-1) {
            buffer_size = atoi(argv[++i]);
        }
    }
    if (port == 0) { port = 8888; }
    if (buffer_size == 0) { buffer_size = 128; }

    TAddress address{"0.0.0.0", port};
    std::cerr << "Method: " << method << "\n";

    if (method == "select") {
        run<TSelect>(debug, address, buffer_size);
    } else if (method == "poll") {
        run<TPoll>(debug, address, buffer_size);
    }
#ifdef __linux__
    else if (method == "epoll") {
        run<TEPoll>(debug, address, buffer_size);
    }
    else if (method == "uring") {
        run<TUring>(debug, address, buffer_size);
    }
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
    else if (method == "kqueue") {
        run<TKqueue>(debug, address, buffer_size);
    }
#endif
    else {
        std::cerr << "Unknown method\n";
    }
    return 0;
}

