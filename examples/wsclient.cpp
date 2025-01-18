#include <coroio/all.hpp>
#include <coroio/ws.hpp>
#include <coroio/resolver.hpp>

#include <regex>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#endif

using namespace NNet;

#ifndef _WIN32
TAddress GetLinkAddress() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        throw std::runtime_error("getifaddrs failed");
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET6) {
            auto* addr6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);

            if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr)
                ||IN6_IS_ADDR_LOOPBACK(&addr6->sin6_addr))
            {
                continue;
            }

            return TAddress(*addr6);
        }
    }

    throw std::runtime_error("No link-local address found");
}
#endif

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
TFuture<void> client(TPoller& poller, const std::string& uri, bool ipv6)
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
        auto addresses = co_await resolver.Resolve(host,
            ipv6 ? EDNSType::AAAA : EDNSType::A
        );

        TAddress address = addresses.front().WithPort(port);
        //TAddress address = TAddress{"127.0.0.1", 8080}; // proxy for test
        std::cerr << "Addr: " << address.ToString() << "\n";
        typename TPoller::TSocket socket(poller, address.Domain());

        if (ipv6) {
#ifndef _WIN32
            TAddress local = GetLinkAddress();
            socket.Bind(local);
#endif
        }

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
void run(const std::string& uri, bool ipv6)
{
    TLoop<TPoller> loop;
    auto h = client(loop.Poller(), uri, ipv6);
    loop.Loop();
}

void usage(const char* name) {
    std::cerr << name << "--uri <uri> [--6]\n";
    std::exit(1);
}

int main(int argc, char** argv) {
    TInitializer init;
    std::string uri;
    bool ipv6 = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
        } else if (!strcmp(argv[i], "--uri") && i < argc-1) {
            uri = argv[++i];
        } else if (!strcmp(argv[i], "--6")) {
            ipv6 = true;
        }
    }
    if (uri.empty()) {
        usage(argv[0]);
    }
    run<TDefaultPoller>(uri, ipv6);
}
