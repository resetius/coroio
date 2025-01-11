#include <coroio/all.hpp>
#include <coroio/ws.hpp>
#include <coroio/resolver.hpp>

using namespace NNet;

template<typename TPoller>
TFuture<void> client(TPoller& poller, const std::string& uri)
{
    std::string host = "ws.kraken.com";
    int port = 443;


//    std::vector<TAddress> addresses;
//    {
//        TResolver<TPollerBase> resolver(poller);
//        co_await resolver.Resolve(host);
//    }

//    TAddress address = addresses.front().WithPort(port);

    TAddress address = TAddress("104.17.187.205", 443);
    std::cerr << "Addr: " << address.ToString() << "\n";
    typename TPoller::TSocket socket(address, poller);
    TSslContext ctx = TSslContext::Client([&](const char* s) { std::cerr << s << "\n"; });
    TSslSocket sslSocket(std::move(socket), ctx);

    co_await sslSocket.Connect();

    TWebSocket ws(sslSocket);

    co_await ws.Connect(host, "/v2");

    auto message = co_await ws.ReceiveText();
    std::cerr << "Message: " << message << "\n";

    co_await ws.SendText(R"__({"method": "subscribe","params":{"channel": "ticker","symbol": ["BTC/USD"]}})__");
    std::cerr << "Send Ok\n";

    try {
        while (true) {
            std::cerr << "Wait ?\n";
            auto message = co_await ws.ReceiveText();
            std::cerr << "Message: " << message << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
}

template<typename TPoller>
void run(const std::string& uri)
{
    TLoop<TPoller> loop;
    auto h = client(loop.Poller(), uri);
    loop.Loop();
}

int main(int argc, char** argv) {
    TInitializer init;
    std::string uri = argv[1];
    run<TDefaultPoller>(uri);
}
