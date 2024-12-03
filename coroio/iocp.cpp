#ifdef _WIN32
#include "iocp.hpp"

namespace NNet {

TIOCp::TIOCp()
    : Port_(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0))
{
    if (Port_ == INVALID_HANDLE_VALUE) {
        throw std::system_error(WSAGetLastError(), std::generic_category(), "CreateIoCompletionPort");
    }
}

TIOCp::~TIOCp()
{
    CloseHandle(Port_);
}

TIOCp::TIO* TIOCp::NewTIO() {
    return new (Allocator_.allocate()) TIO();
}

void TIOCp::FreeTIO(TIO* tio) {
    Allocator_.deallocate(tio);
}

void TIOCp::Recv(int fd, void* buf, int size, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO();
    tio->event.Fd = fd;
    tio->event.Handle = handle;
    tio->event.Type = TEvent::READ;
    WSABUF recvBuf = {(ULONG)size, (char*)buf};
    DWORD flags = 0;
    DWORD outSize = 0;
    auto ret = WSARecv((SOCKET)fd, &recvBuf, 1, &outSize, &flags, (WSAOVERLAPPED*)tio, nullptr);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "WSARecv");
    }
}

void TIOCp::Send(int fd, const void* buf, int size, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO();
    tio->event.Fd = fd;
    tio->event.Handle = handle;
    tio->event.Type = TEvent::WRITE;
    WSABUF sendBuf = {(ULONG)size, (char*)buf};
    DWORD outSize = 0;
    auto ret = WSASend((SOCKET)fd, &sendBuf, 1, &outSize, 0, (WSAOVERLAPPED*)tio, nullptr);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "WSARecv");
    }
}

void TIOCp::Accept(int fd, struct sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO();
    tio->event.Fd = fd;
    tio->event.Handle = handle;
    tio->event.Type = TEvent::READ;
    tio->addr = addr;
    tio->len = len;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    tio->sock = (int)sock;
    if (sock == INVALID_SOCKET) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "socket");
    }
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        FreeTIO(tio);
        closesocket(sock);
        throw std::system_error(WSAGetLastError(), std::system_category(), "ioctlsocket");
    }

    DWORD dwBytesReceived = 0;
    auto ret = AcceptEx((SOCKET)fd, sock, addr, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16, &dwBytesReceived, (WSAOVERLAPPED*)tio);
    if (ret == FALSE && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        closesocket(sock);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "AcceptEx");
    }
}

void TIOCp::Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO();
    tio->event.Fd = fd;
    tio->event.Handle = handle;
    tio->event.Type = TEvent::READ;

    int bind_rc = 0;
    if (len == sizeof(sockaddr_in)) {
        struct sockaddr_in tmp;
        ZeroMemory(&tmp, sizeof(tmp));
        tmp.sin_family = AF_INET;
        tmp.sin_addr.s_addr = INADDR_ANY;
        tmp.sin_port = 0;
        bind((SOCKET)fd, (SOCKADDR*) &tmp, sizeof(tmp));
    } else {
        sockaddr_in6 tmp;
        ZeroMemory(&tmp, sizeof(tmp));
        tmp.sin6_family = AF_INET6;
        tmp.sin6_addr = in6addr_any;
        tmp.sin6_port = 0;
        bind((SOCKET)fd, (SOCKADDR*) &tmp, sizeof(tmp));
    }

    if (bind_rc != 0) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "bind");
    }

    auto ret = ConnectEx((SOCKET)fd, addr, len, nullptr, 0, nullptr, (WSAOVERLAPPED*)tio);
    if (ret == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "ConnectEx");
    }
    if (ret == TRUE) {
        PostQueuedCompletionStatus(Port_, 0, (ULONG_PTR)fd /*key*/, (WSAOVERLAPPED*)tio);
    }
}

void TIOCp::Read(int fd, void* buf, int size, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO();
    tio->event.Fd = fd;
    tio->event.Handle = handle;
    tio->event.Type = TEvent::READ;

    auto ret = ReadFile(reinterpret_cast<HANDLE>(fd), buf, size, nullptr, (WSAOVERLAPPED*)tio);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "ReadFile");
    }
}

void TIOCp::Write(int fd, const void* buf, int size, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO();
    tio->event.Fd = fd;
    tio->event.Handle = handle;
    tio->event.Type = TEvent::WRITE;

    auto ret = WriteFile(reinterpret_cast<HANDLE>(fd), buf, size, nullptr, (WSAOVERLAPPED*)tio);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "WriteFile");
    }
}

void TIOCp::Register(int fd) {
    CreateIoCompletionPort((HANDLE)(SOCKET)fd, Port_, (ULONG_PTR)fd, 0);
}

void TIOCp::Cancel([[maybe_unused]] int fd)
{
    // TODO: implement
}

int TIOCp::Result() {
    return Result_;
}

long TIOCp::GetTimeoutMs() {
    auto ts = GetTimeout();
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void TIOCp::Poll()
{
    DWORD size = 0;
    void* completionKey;
    TIO* event;

    while (GetQueuedCompletionStatus(Port_, &size, (PULONG_PTR)&completionKey, (LPOVERLAPPED*)&event, GetTimeoutMs()) == TRUE) {
        Result_ = size;
        if (event->addr) {
            sockaddr_in* remoteAddr = nullptr;
            sockaddr_in* localAddr = nullptr;
            int localAddrLen = 0;
            *event->len = 0;
            Result_ = event->sock;
            GetAcceptExSockaddrs(event->addr, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16, (sockaddr**)&localAddr, &localAddrLen, (sockaddr**)&remoteAddr, event->len);
            memmove(event->addr, remoteAddr, *event->len);
        }
        event->event.Handle.resume();
        FreeTIO(event);
    }

    ProcessTimers();
}

}

#endif // _WIN32