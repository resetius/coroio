#include <all.hpp>

#include <signal.h>

using NNet::TSocket;
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

template<bool debug, typename TPoller>
NNet::TTestTask client(TPoller& poller, TAddress addr, int buffer_size)
{
    using TSocket = typename TPoller::TSocket;
    std::vector<char> out(buffer_size);
    std::vector<char> in(buffer_size);
    ssize_t size = 1;

    try {
        TSocket input{TAddress{}, 0, poller}; // stdin
        TSocket socket{std::move(addr), poller};

        co_await socket.Connect();
        while (size && (size = co_await input.ReadSome(out.data(), out.size()))) {
            co_await socket.WriteSome(out.data(), size);
            size = co_await socket.ReadSome(in.data(), in.size());
            if constexpr(debug) {
                std::cout << "Received: " << std::string_view(in.data(), size) << "\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }

    co_return;
}

template<typename TPoller>
void run(bool debug, TAddress address, int buffer_size)
{
    NNet::TLoop<TPoller> loop;
    NNet::THandle h;
    if (debug) {
        h = client<true>(loop.Poller(), std::move(address), buffer_size);
    } else {
        h = client<false>(loop.Poller(), std::move(address), buffer_size);
    }
    while (!h.done()) {
        loop.Step();
    }
    h.destroy();
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::string addr;
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

    TAddress address{addr, port};
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
