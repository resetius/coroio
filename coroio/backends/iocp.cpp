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

void TIOCp::Prepare(TOperation& op, int fd) {
    op.Reset(fd);
}

void TIOCp::Throw(TOperation& op, int err, const std::error_category& category, const char* what) {
    op.Handle = {};
    throw std::system_error(err, category, what);
}

void TIOCp::Recv(int fd, void* buf, int size, TOperation& op)
{
    Prepare(op, fd);
    WSABUF recvBuf = {(ULONG)size, (char*)buf};
    DWORD flags = 0;
    DWORD outSize = 0;
    auto ret = WSARecv((SOCKET)fd, &recvBuf, 1, &outSize, &flags, static_cast<OVERLAPPED*>(&op), nullptr);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        Throw(op, WSAGetLastError(), std::generic_category(), "WSARecv");
    }
    InFlight_++;
}

void TIOCp::Send(int fd, const void* buf, int size, TOperation& op)
{
    Prepare(op, fd);
    WSABUF sendBuf = {(ULONG)size, (char*)buf};
    DWORD outSize = 0;
    auto ret = WSASend((SOCKET)fd, &sendBuf, 1, &outSize, 0, static_cast<OVERLAPPED*>(&op), nullptr);
    if (ret != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        Throw(op, WSAGetLastError(), std::generic_category(), "WSARecv");
    }
    InFlight_++;
}

void TIOCp::Accept(int fd, struct sockaddr* addr, socklen_t* len, TOperation& op)
{
    Prepare(op, fd);
    op.Addr = addr;
    op.Len = len;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    op.Sock = (int)sock;
    if (sock == INVALID_SOCKET) {
        Throw(op, WSAGetLastError(), std::generic_category(), "socket");
    }
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        closesocket(sock);
        Throw(op, WSAGetLastError(), std::system_category(), "ioctlsocket");
    }

    DWORD dwBytesReceived = 0;
    auto ret = AcceptEx((SOCKET)fd, sock, addr, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16, &dwBytesReceived, static_cast<OVERLAPPED*>(&op));
    if (ret == FALSE && WSAGetLastError() != WSA_IO_PENDING) {
        closesocket(sock);
        Throw(op, WSAGetLastError(), std::generic_category(), "AcceptEx");
    }
    InFlight_++;
}

void TIOCp::Connect(int fd, const sockaddr* addr, socklen_t len, TOperation& op)
{
    Prepare(op, fd);
    op.Connect = true;

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
        Throw(op, WSAGetLastError(), std::generic_category(), "bind");
    }

    auto ret = ConnectEx((SOCKET)fd, addr, len, nullptr, 0, nullptr, static_cast<OVERLAPPED*>(&op));
    if (ret == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
        Throw(op, WSAGetLastError(), std::generic_category(), "ConnectEx");
    }
    InFlight_++;
}

void TIOCp::Read(int fd, void* buf, int size, TOperation& op)
{
    Prepare(op, fd);
    op.File = true;

    auto ret = ReadFile(reinterpret_cast<HANDLE>(fd), buf, size, nullptr, static_cast<OVERLAPPED*>(&op));
    if (!ret && WSAGetLastError() != WSA_IO_PENDING) {
        Throw(op, WSAGetLastError(), std::generic_category(), "ReadFile");
    }
    InFlight_++;
}

void TIOCp::Write(int fd, const void* buf, int size, TOperation& op)
{
    Prepare(op, fd);
    op.File = true;

    auto ret = WriteFile(reinterpret_cast<HANDLE>(fd), buf, size, nullptr, static_cast<OVERLAPPED*>(&op));
    if (!ret && WSAGetLastError() != WSA_IO_PENDING) {
        Throw(op, WSAGetLastError(), std::generic_category(), "WriteFile");
    }
    InFlight_++;
}

void TIOCp::Register(int fd) {
    CreateIoCompletionPort((HANDLE)(SOCKET)fd, Port_, (ULONG_PTR)fd, 0);
}

void TIOCp::Cancel(int fd)
{
    CancelIoEx((HANDLE)(SOCKET)fd, nullptr);
}

int TIOCp::Cancel(TOperation& op)
{
    op.Handle = {};
    if (Completions_.Drop(op)) {
        return op.Result;
    }

    CancelIoEx((HANDLE)(SOCKET)op.Fd, &op);
    // The system may use the operation's buffers until it completes, and its packet points
    // to op, so wait until the packet is dequeued. A file handle is not associated with the
    // port and gets no packet: wait until its OVERLAPPED is no longer pending.
    // Other completions are kept for the next Poll().
    bool done = false;
    while (!done && !(op.File && HasOverlappedIoCompleted(&op))) {
        Entries_.resize(std::max(InFlight_, 1));
        DWORD fired = 0;
        if (!GetQueuedCompletionStatusEx(Port_, &Entries_[0], Entries_.size(), &fired, 10, FALSE)) {
            if (GetLastError() == WAIT_TIMEOUT) {
                continue;
            }
            break; // called from destructors: do not throw
        }
        for (DWORD i = 0; i < fired; i++) {
            auto* other = static_cast<TOperation*>(Entries_[i].lpOverlapped);
            int result = Complete(Entries_[i]);
            if (other == &op) {
                done = true;
            } else {
                Completions_.Reap(other, result);
            }
        }
    }
    return -1;
}

void TIOCp::WakeupReadyHandles() {
    Completions_.Wakeup();
}

long TIOCp::GetTimeoutMs() {
    auto ts = GetTimeout();
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int TIOCp::Complete(const OVERLAPPED_ENTRY& entry)
{
    TOperation* op = static_cast<TOperation*>(entry.lpOverlapped);
    InFlight_--;
    int result = -1;
    if (!op->Handle) {
        // Cancelled: nobody takes the socket, accepted or not.
        if (op->Addr) {
            closesocket((SOCKET)op->Sock);
        }
    } else if (op->Addr) {
        sockaddr_in* remoteAddr = nullptr;
        sockaddr_in* localAddr = nullptr;
        int localAddrLen = 0;
        *op->Len = 0;
        GetAcceptExSockaddrs(op->Addr, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16, (sockaddr**)&localAddr, &localAddrLen, (sockaddr**)&remoteAddr, op->Len);
        memmove(op->Addr, remoteAddr, *op->Len);
        result = op->Sock;
    } else if (op->Connect) {
        // ConnectEx transfers no bytes, its outcome is the operation status.
        SOCKET sock = (SOCKET)op->Fd;
        DWORD transferred = 0;
        DWORD flags = 0;
        result = 0;
        if (!WSAGetOverlappedResult(sock, op, &transferred, FALSE, &flags)
            || setsockopt(sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0)
        {
            result = -WSAGetLastError();
        }
    } else {
        result = entry.dwNumberOfBytesTransferred;
    }
    return result;
}

void TIOCp::Poll()
{
    for (auto& change : Changes_) {
        if (change.Type & TEvent::CLOSE) {
            // closesocket() aborts the pending overlapped operations, whose packets still reach the port.
            CloseDescriptor(change);
        }
    }
    Reset();
    // Completions reaped by Cancel() or not delivered yet are ready already: do not block.
    DWORD timeout = Completions_.Carry() ? 0 : GetTimeoutMs();
    Entries_.resize(std::max(InFlight_, 1));

    DWORD fired = 0;
    auto res = GetQueuedCompletionStatusEx(Port_, &Entries_[0], Entries_.size(), &fired, timeout, FALSE);
    if (res == FALSE && GetLastError() != WAIT_TIMEOUT) {
        throw std::system_error(GetLastError(), std::generic_category(), "GetQueuedCompletionStatusEx");
    }

    for (DWORD i = 0; i < fired && res == TRUE; i++) {
        auto* op = static_cast<TOperation*>(Entries_[i].lpOverlapped);
        Completions_.Collect(op, Complete(Entries_[i]));
    }

    ProcessTimers();
}

}

#endif // _WIN32
