#include "init.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Mswsock.h> // for ConnectEx, AcceptEx
#include <io.h>
#endif

#ifndef _WIN32
#include <signal.h>
#endif

#include <stdexcept>
#include <string>

namespace NNet {

#ifdef _WIN32
LPFN_CONNECTEX ConnectEx;
LPFN_ACCEPTEX AcceptEx;
LPFN_GETACCEPTEXSOCKADDRS GetAcceptExSockaddrs;
#endif

TInitializer::TInitializer() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#else
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with error: " + std::to_string(result));
    }

    auto sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        throw std::runtime_error("Cannot initialize dummy socket");
    }
    GUID guid = WSAID_CONNECTEX;
    DWORD dwBytes = 0;
    auto res = WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &ConnectEx, sizeof(ConnectEx), &dwBytes, nullptr, nullptr);
    if (res != 0) {
        closesocket(sock);
        throw std::runtime_error("Cannot query dummy socket");
    }

    guid = WSAID_ACCEPTEX;
    res = WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &AcceptEx, sizeof(AcceptEx), &dwBytes, nullptr, nullptr);
    if (res != 0) {
        closesocket(sock);
        throw std::runtime_error("Cannot query dummy socket");
    }

    guid = WSAID_GETACCEPTEXSOCKADDRS;
    res = WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &GetAcceptExSockaddrs, sizeof(GetAcceptExSockaddrs), &dwBytes, nullptr, nullptr);
    if (res != 0) {
        closesocket(sock);
        throw std::runtime_error("Cannot query dummy socket");
    }

    closesocket(sock);
#endif
}

#ifdef _WIN32
TInitializer::~TInitializer() {
    WSACleanup();
}
#endif

} // namespace NNet