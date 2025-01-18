#include <coroio/all.hpp>
#include <coroio/ws.hpp>
#include <coroio/resolver.hpp>

#include <regex>

using namespace NNet;

template<typename TWebSocket>
TFuture<void> reader(TWebSocket& ws) {
    try {
        while (true) {
            auto message = co_await ws.ReceiveText();
            std::cerr << "Message: " << message << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

template<typename TSocket, typename TPoller>
TFuture<void> client(TSocket&& socket, TAddress& address, TPoller& poller, std::string host, std::string path)
{
    constexpr int maxLineSize = 4096;
    co_await socket.Connect(address);

    TWebSocket ws(socket);

    co_await ws.Connect(host, path);
    auto r = reader(ws);

    TFileHandle input{0, poller}; // stdin
    TLineReader lineReader(input, maxLineSize);
    while (auto line = co_await lineReader.Read()) {
        std::string str = std::string(line.Part1);
        str += line.Part2;
        co_await ws.SendText(std::string_view(str));
    }

    co_await r;
    co_return;
}

template<typename TPoller>
TFuture<void> client(TPoller& poller, const std::string& uri)
{
    try {
        std::regex wsRegex(R"(^(wss?|ws)://([^:/?#]+)(?::(\d+))?([^?#]*)(\?[^#]*)?)");
        std::smatch match;
        if (!std::regex_match(uri, match, wsRegex)) {
            throw std::invalid_argument("Invalid WebSocket URI format");
        }

        std::string host = match[2].str();
        int port = match[1].str() == "wss" ? 443 : 80;
        if (match[3].matched) {
            port = std::stoi(match[3].str());
        }
        std::string path = match[4].str();

        TResolver<TPollerBase> resolver(poller);
        auto addresses = co_await resolver.Resolve(host);

        TAddress address = addresses.front().WithPort(port);
        //TAddress address = TAddress{"127.0.0.1", 8080}; // proxy for test
        std::cerr << "Addr: " << address.ToString() << "\n";
        typename TPoller::TSocket socket(poller, address.Domain());

        if (port == 443) {
            TSslContext ctx = TSslContext::Client([&](const char* s) { std::cerr << s << "\n"; });
            TSslSocket sslSocket(std::move(socket), ctx);
            sslSocket.SslSetTlsExtHostName(host);

            co_await client(std::move(sslSocket), address, poller, std::move(host), std::move(path));
        } else {
            co_await client(std::move(socket), address, poller, std::move(host), std::move(path));
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
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <uri>\n";
        return 1;
    }
    std::string uri = argv[1];
    run<TDefaultPoller>(uri);
}
