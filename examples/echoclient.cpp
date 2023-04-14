#include <net.hpp>
#include <signal.h>

using NNet::TSimpleTask;
using NNet::TSocket;
using NNet::TSelect;
using NNet::TAddress;
using TLoop = NNet::TLoop<TSelect>;

TSimpleTask client(TLoop* loop, TAddress addr)
{
    char out[128] = {0};
    char in[128] = {0};
    ssize_t size = 1;

    try {
        TSocket input{TAddress{}, 0, loop->Poller()}; // stdin
        TSocket socket{addr, loop->Poller()};

        co_await socket.Connect();
        while (size && (size = co_await input.ReadSome(out, sizeof(out)))) {
            co_await socket.WriteSome(out, size);
            size = co_await socket.ReadSome(in, sizeof(in));
            std::cout << "Received: " << std::string_view(in, size) << "\n";
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }
    loop->Stop();
    co_return;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::string addr;
    int port = 0;
    if (argc > 1) { addr = argv[1]; }
    if (addr.empty()) { addr = "127.0.0.1"; }
    if (argc > 2) { port = atoi(argv[2]); }
    if (port == 0) { port = 8888; }

    TAddress address{addr, port};
    TLoop loop;
    client(&loop, std::move(address));
    loop.Loop();
    return 0;
}
