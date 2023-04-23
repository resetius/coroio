#include <net.hpp>
#include <select.hpp>
#include <poll.hpp>
#include <socket.hpp>

#include <signal.h>

#ifdef __linux__
#include <epoll.hpp>
#include <uring.hpp>
#endif

using NNet::TSimpleTask;
using NNet::TSocket;
using NNet::TSelect;
using NNet::TEPoll;
using NNet::TAddress;
using NNet::TUring;
using TLoop = NNet::TLoop<TEPoll>;

template<bool debug>
TSimpleTask client_handler(TSocket socket, int buffer_size) {
    std::vector<char> buffer(buffer_size); ssize_t size = 0;

    try {
        while ((size = co_await socket.ReadSome(buffer.data(), buffer_size)) > 0) {
            if constexpr(debug) {
                std::cerr << "Received: " << std::string_view(buffer.data(), size) << "\n";
            }
            co_await socket.WriteSome(buffer.data(), size);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    if (size == 0) {
        std::cerr << "Client disconnected\n";
    }
    co_return;
}

template<bool debug>
TSimpleTask server(TLoop* loop, TAddress address, int buffer_size)
{
    TSocket socket(std::move(address), loop->Poller());
    socket.Bind();
    socket.Listen();

    while (true) {
        auto client = co_await socket.Accept();
        client_handler<debug>(std::move(client), buffer_size);
    }
    co_return;
}

template<bool debug>
TSimpleTask client_handler_ur(NNet::TUringSocket socket, int buffer_size) {
    std::vector<char> buffer(buffer_size); ssize_t size = 0;

    try {
        while ((size = co_await socket.ReadSome(buffer.data(), buffer_size)) > 0) {
            if constexpr(debug) {
                std::cerr << "Received: " << std::string_view(buffer.data(), size) << "\n";
            }
            co_await socket.WriteSome(buffer.data(), size);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    if (size == 0) {
        std::cerr << "Client disconnected\n";
    }
    co_return;
}

template<bool debug>
TSimpleTask server_ur(TUring* uring, TAddress address, int buffer_size)
{
    NNet::TUringSocket socket(std::move(address), *uring);
    socket.Bind();
    socket.Listen();

    while (true) {
        auto client = co_await socket.Accept();
        client_handler_ur<debug>(std::move(client), buffer_size);
    }
    co_return;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    int port = 0;
    int buffer_size = 128;
    bool uring = false;
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i < argc-1) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--uring")) {
            uring = true;
        } else if (!strcmp(argv[i], "--debug")) {
            debug = true;
        } else if (!strcmp(argv[i], "--buffer-size") && i < argc-1) {
            buffer_size = atoi(argv[++i]);
        }
    }
    if (port == 0) { port = 8888; }
    if (buffer_size == 0) { buffer_size = 128; }

    TAddress address{"0.0.0.0", port};
#ifdef __linux__
    if (uring) {
        std::cout << "Using uring \n";
        NNet::TLoop<NNet::TUring> loop;
        if (debug) {
            server_ur<true>(&loop.Poller(), std::move(address), buffer_size);
        } else {
            server_ur<false>(&loop.Poller(), std::move(address), buffer_size);
        }
        loop.Loop();
    } else
#endif
    {
        TLoop loop;
        if (debug) {
            server<true>(&loop, std::move(address), buffer_size);
        } else {
            server<false>(&loop, std::move(address), buffer_size);
        }
        loop.Loop();
    }
    return 0;
}

