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
    DWORD outSize = 0;
    auto ret = WSARecv((SOCKET)fd, &recvBuf, 1, &outSize, nullptr, (WSAOVERLAPPED*)tio, nullptr);
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
    tio->event.Type = TEvent::READ;
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
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "socket");
    }

    DWORD dwBytesReceived = 0;
    auto ret = AcceptEx((SOCKET)fd, sock, addr, 0, *len, *len, &dwBytesReceived, (WSAOVERLAPPED*)tio);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
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

    DWORD dwBytesSent = 0;
    auto ret = ConnectEx((SOCKET)fd, addr, len, nullptr, 0, &dwBytesSent, (WSAOVERLAPPED*)tio);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "AcceptEx");
    }
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
        ;
    }

    ProcessTimers();
}

}

#endif // _WIN32