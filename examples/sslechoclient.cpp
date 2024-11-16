#include <coroio/all.hpp>

#include <signal.h>

using namespace NNet;

template<bool debug, typename TPoller>
TFuture<void> client(TPoller& poller, TSslContext& ctx, TAddress addr)
{
    static constexpr int maxLineSize = 4096;
    using TSocket = typename TPoller::TSocket;
    using TFileHandle = typename TPoller::TFileHandle;
    std::vector<char> in(maxLineSize);

    try {
        TFileHandle input{0, poller}; // stdin
        TSocket socket{std::move(addr), poller};
        TSslSocket sslSocket(std::move(socket), ctx);
        TLineReader lineReader(input, maxLineSize);
        TByteWriter byteWriter(sslSocket);
        TByteReader byteReader(sslSocket);

        co_await sslSocket.Connect();
        while (auto line = co_await lineReader.Read()) {
            co_await byteWriter.Write(line);
            co_await byteReader.Read(in.data(), line.Size());
            if constexpr(debug) {
                std::cout << "Received: " << std::string_view(in.data(), line.Size()) << "\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }

    co_return;
}

template<typename TPoller>
void run(bool debug, TAddress address)
{
    NNet::TLoop<TPoller> loop;
    NNet::TFuture<void> h;
    if (debug) {
        TSslContext ctx = TSslContext::Client([&](const char* s) { std::cerr << s << "\n"; });
        h = client<true>(loop.Poller(), ctx, std::move(address));
    } else {
        TSslContext ctx = TSslContext::Client();
        h = client<false>(loop.Poller(), ctx, std::move(address));
    }
    while (!h.done()) {
        loop.Step();
    }
}

int main(int argc, char** argv) {
    TInitializer init;
    std::string addr;
    int port = 0;
    std::string method = "select";
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i < argc-1) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--method") && i < argc-1) {
            method = argv[++i];
        } else if (!strcmp(argv[i], "--debug")) {
            debug = true;
        }
    }
    if (port == 0) { port = 8888; }

    TAddress address{addr, port};
    std::cerr << "Method: " << method << "\n";

    if (method == "select") {
        run<TSelect>(debug, address);
    }
#ifndef _WIN32
    else if (method == "poll") {
        run<TPoll>(debug, address);
    }
#endif
#ifdef __linux__
    else if (method == "epoll") {
        run<TEPoll>(debug, address);
    }
    else if (method == "uring") {
        run<TUring>(debug, address);
    }
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
    else if (method == "kqueue") {
        run<TKqueue>(debug, address);
    }
#endif
    else {
        std::cerr << "Unknown method\n";
    }

    return 0;
}
