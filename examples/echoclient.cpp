#include <coroio/all.hpp>

#include <signal.h>

using namespace NNet;

template<bool debug, typename TPoller>
TFuture<void> client(TPoller& poller, TAddress addr)
{
    static constexpr int maxLineSize = 4096;
    using TSocket = typename TPoller::TSocket;
    using TFileHandle = typename TPoller::TFileHandle;
    std::vector<char> in(maxLineSize);

    try {
        TFileHandle input{0, poller}; // stdin
        TSocket socket{poller, addr.Domain()};
        TLineReader lineReader(input, maxLineSize);
        TByteWriter byteWriter(socket);
        TByteReader byteReader(socket);

        // Windows unable to detect 'connection refused' without setting timeout
        co_await socket.Connect(addr, TClock::now()+std::chrono::milliseconds(1000));
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
    TLoop<TPoller> loop;
    TFuture<void> h;
    if (debug) {
        h = client<true>(loop.Poller(), std::move(address));
    } else {
        h = client<false>(loop.Poller(), std::move(address));
    }
    while (!h.done()) {
        loop.Step();
    }
}

void usage(const char* name) {
    std::cerr << name << " [--port 80] [--addr 127.0.0.1] [--method select|poll|epoll|kqueue] [--debug] [--help]" << std::endl;
    std::exit(1);
}

int main(int argc, char** argv) {
    TInitializer init;
    std::string addr = "127.0.0.1";
    int port = 0;
    std::string method = "select";
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i < argc-1) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--addr") && i < argc-1) {
            addr = argv[++i];
        } else if (!strcmp(argv[i], "--method") && i < argc-1) {
            method = argv[++i];
        } else if (!strcmp(argv[i], "--debug")) {
            debug = true;
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
        }
    }
    if (port == 0) { port = 8888; }

    TAddress address{addr, port};
    std::cerr << "Method: " << method << "\n";

    if (method == "select") {
        run<TSelect>(debug, address);
    }
    else if (method == "poll") {
        run<TPoll>(debug, address);
    }
#ifdef HAVE_EPOLL
    else if (method == "epoll") {
        run<TEPoll>(debug, address);
    }
#endif
#ifdef HAVE_URING
    else if (method == "uring") {
        run<TUring>(debug, address);
    }
#endif
#ifdef HAVE_KQUEUE
    else if (method == "kqueue") {
        run<TKqueue>(debug, address);
    }
#endif
#ifdef HAVE_IOCP
    else if (method == "iocp") {
        run<TIOCp>(debug, address);
    }
#endif
    else {
        std::cerr << "Unknown method\n";
    }

    return 0;
}
