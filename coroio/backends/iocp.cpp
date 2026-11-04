#ifdef _WIN32
#include "iocp.hpp"

#include <Mswsock.h> // for ConnectEx, AcceptEx

namespace NNet {

extern LPFN_CONNECTEX ConnectEx;
extern LPFN_ACCEPTEX AcceptEx;
extern LPFN_GETACCEPTEXSOCKADDRS GetAcceptExSockaddrs;

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

TIOCp::TIO* TIOCp::NewTIO(int fd, THandle handle) {
    TIO* tio = new (Allocator_.Allocate()) TIO();
    tio->fd = fd;
    tio->handle = handle;
    tio->next = Pending_;
    if (Pending_) {
        Pending_->prev = tio;
    }
    Pending_ = tio;
    return tio;
}

void TIOCp::FreeTIO(TIO* tio) {
    if (tio->prev) {
        tio->prev->next = tio->next;
    } else {
        Pending_ = tio->next;
    }
    if (tio->next) {
        tio->next->prev = tio->prev;
    }
    Allocator_.Deallocate(tio);
}

void TIOCp::Recv(int fd, void* buf, int size, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO(fd, handle);
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
    TIO* tio = NewTIO(fd, handle);
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
    TIO* tio = NewTIO(fd, handle);
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
    TIO* tio = NewTIO(fd, handle);
    tio->connect = true;

    int bind_result = 0;
    if (len == sizeof(sockaddr_in)) {
        struct sockaddr_in tmp;
        ZeroMemory(&tmp, sizeof(tmp));
        tmp.sin_family = AF_INET;
        tmp.sin_addr.s_addr = INADDR_ANY;
        tmp.sin_port = 0;
        bind_result = bind((SOCKET)fd, (SOCKADDR*) &tmp, sizeof(tmp));
    } else {
        sockaddr_in6 tmp;
        ZeroMemory(&tmp, sizeof(tmp));
        tmp.sin6_family = AF_INET6;
        tmp.sin6_addr = in6addr_any;
        tmp.sin6_port = 0;
        bind_result = bind((SOCKET)fd, (SOCKADDR*) &tmp, sizeof(tmp));
    }

    if (bind_result != 0) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "bind");
    }

    auto ret = ConnectEx((SOCKET)fd, addr, len, nullptr, 0, nullptr, (WSAOVERLAPPED*)tio);
    if (ret == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "ConnectEx");
    }
}

void TIOCp::Read(int fd, void* buf, int size, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO(fd, handle);

    auto ret = ReadFile(reinterpret_cast<HANDLE>(fd), buf, size, nullptr, (WSAOVERLAPPED*)tio);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "ReadFile");
    }
}

void TIOCp::Write(int fd, const void* buf, int size, std::coroutine_handle<> handle)
{
    TIO* tio = NewTIO(fd, handle);

    auto ret = WriteFile(reinterpret_cast<HANDLE>(fd), buf, size, nullptr, (WSAOVERLAPPED*)tio);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        FreeTIO(tio);
        throw std::system_error(WSAGetLastError(), std::generic_category(), "WriteFile");
    }
}

void TIOCp::Register(int fd) {
    CreateIoCompletionPort((HANDLE)(SOCKET)fd, Port_, (ULONG_PTR)fd, 0);
}

void TIOCp::Cancel(int fd)
{
    CancelIoEx((HANDLE)(SOCKET)fd, nullptr);
}

int TIOCp::Cancel(THandle h)
{
    for (size_t i = 0; i < ReadyEvents_.size(); i++) {
        if (ReadyEvents_[i].Handle == h) {
            ReadyEvents_[i].Handle = {};
            return Results_[i];
        }
    }
    for (TIO* tio = Pending_; tio; tio = tio->next) {
        if (tio->handle == h) {
            // Poll() frees it when its completion arrives, without resuming h.
            tio->handle = {};
            CancelIoEx((HANDLE)(SOCKET)tio->fd, &tio->overlapped);
            break;
        }
    }
    return -1;
}

int TIOCp::Result() {
    return Results_[NextResult_++];
}

void TIOCp::WakeupReadyHandles() {
    for (size_t i = 0; i < ReadyEvents_.size(); i++) {
        // Cleared before resuming, so that Cancel() finds only completions not delivered yet.
        if (auto h = std::exchange(ReadyEvents_[i].Handle, {})) {
            NextResult_ = i;
            h.resume();
        }
    }
}

long TIOCp::GetTimeoutMs() {
    auto ts = GetTimeout();
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void TIOCp::Poll()
{
    Reset();
    Results_.clear();
    NextResult_ = 0;
    Entries_.resize(std::max(Allocator_.Count(), 1));

    DWORD fired = 0;
    auto res = GetQueuedCompletionStatusEx(Port_, &Entries_[0], Entries_.size(), &fired, GetTimeoutMs(), FALSE);
    if (res == FALSE && GetLastError() != WAIT_TIMEOUT) {
        throw std::system_error(GetLastError(), std::generic_category(), "GetQueuedCompletionStatusEx");
    }

    for (DWORD i = 0; i < fired && res == TRUE; i++) {
        TIO* event = (TIO*)Entries_[i].lpOverlapped;
        if (!event->handle) {
            // Cancelled: its coroutine is gone, and so is the memory addr and len point to.
            if (event->addr) {
                closesocket((SOCKET)event->sock);
            }
            FreeTIO(event);
            continue;
        }
        if (event->addr) {
            sockaddr_in* remoteAddr = nullptr;
            sockaddr_in* localAddr = nullptr;
            int localAddrLen = 0;
            *event->len = 0;
            GetAcceptExSockaddrs(event->addr, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16, (sockaddr**)&localAddr, &localAddrLen, (sockaddr**)&remoteAddr, event->len);
            memmove(event->addr, remoteAddr, *event->len);
            Results_.push_back(event->sock);
        } else if (event->connect) {
            // ConnectEx transfers no bytes, its outcome is the operation status.
            SOCKET sock = (SOCKET)event->fd;
            DWORD transferred = 0;
            DWORD flags = 0;
            int ret = 0;
            if (!WSAGetOverlappedResult(sock, &event->overlapped, &transferred, FALSE, &flags)
                || setsockopt(sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0)
            {
                ret = -WSAGetLastError();
            }
            Results_.push_back(ret);
        } else {
            Results_.push_back(Entries_[i].dwNumberOfBytesTransferred);
        }
        ReadyEvents_.emplace_back(TEvent{-1, 0, event->handle});
        FreeTIO(event);
    }

    ProcessTimers();
}

}

#endif // _WIN32
