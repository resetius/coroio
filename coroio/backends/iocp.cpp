#ifdef _WIN32
#include "iocp.hpp"

#include <Mswsock.h> // for ConnectEx, AcceptEx
#include <io.h> // for _get_osfhandle

namespace NNet {

extern LPFN_CONNECTEX ConnectEx;
extern LPFN_ACCEPTEX AcceptEx;
extern LPFN_GETACCEPTEXSOCKADDRS GetAcceptExSockaddrs;

namespace {

// Winsock errors of the NTSTATUS codes a failed socket operation completes with, the same
// as WSAGetOverlappedResult() gives. It cannot be used: it fails once the socket is closed.
constexpr std::pair<ULONG_PTR, int> SocketErrors[] = {
    {0xC0000120, WSA_OPERATION_ABORTED}, // STATUS_CANCELLED
    {0xC000020D, WSAECONNRESET},         // STATUS_CONNECTION_RESET
    {0xC000013C, WSAECONNRESET},         // STATUS_REMOTE_DISCONNECT
    {0xC000020C, WSAECONNRESET},         // STATUS_CONNECTION_DISCONNECTED
    {0xC000023F, WSAECONNRESET},         // STATUS_PORT_UNREACHABLE
    {0xC0000241, WSAECONNABORTED},       // STATUS_CONNECTION_ABORTED
    {0xC000013B, WSAECONNABORTED},       // STATUS_LOCAL_DISCONNECT
    {0xC0000236, WSAECONNREFUSED},       // STATUS_CONNECTION_REFUSED
    {0xC00000B5, WSAETIMEDOUT},          // STATUS_IO_TIMEOUT
    {0xC000023C, WSAENETUNREACH},        // STATUS_NETWORK_UNREACHABLE
    {0xC000023D, WSAEHOSTUNREACH},       // STATUS_HOST_UNREACHABLE
    {0xC000009A, WSAENOBUFS},            // STATUS_INSUFFICIENT_RESOURCES
    {0x80000005, WSAEMSGSIZE},           // STATUS_BUFFER_OVERFLOW
};

bool Failed(const OVERLAPPED& op) {
    return static_cast<LONG>(op.Internal) < 0; // !NT_SUCCESS
}

int SocketError(const OVERLAPPED& op) {
    for (auto [status, error] : SocketErrors) {
        if (static_cast<ULONG>(op.Internal) == status) {
            return error;
        }
    }
    return WSAEINVAL; // statuses not in the table; Winsock maps many more
}

} // namespace

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
    if (fd < 0) {
        Throw(op, EBADF, std::generic_category(), "accept");
    }
    sockaddr_storage local = {};
    int localLen = sizeof(local);
    if (getsockname((SOCKET)fd, reinterpret_cast<sockaddr*>(&local), &localLen) != 0) {
        Throw(op, WSAGetLastError(), std::generic_category(), "getsockname");
    }
    // AcceptEx needs a socket of the listener's family, e.g. AF_INET6 for an IPv6 connection.
    SOCKET sock = socket(local.ss_family, SOCK_STREAM, IPPROTO_TCP);
    op.Sock = (int)sock;
    if (sock == INVALID_SOCKET) {
        Throw(op, WSAGetLastError(), std::generic_category(), "socket");
    }
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        int err = WSAGetLastError(); // closesocket() resets it
        closesocket(sock);
        Throw(op, err, std::generic_category(), "ioctlsocket");
    }

    DWORD dwBytesReceived = 0;
    auto ret = AcceptEx((SOCKET)fd, sock, addr, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16, &dwBytesReceived, static_cast<OVERLAPPED*>(&op));
    int err = ret == FALSE ? WSAGetLastError() : 0; // closesocket() resets it
    if (ret == FALSE && err != WSA_IO_PENDING) {
        closesocket(sock);
        Throw(op, err, std::generic_category(), "AcceptEx");
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

HANDLE TIOCp::StartFile(int fd, int size, TOperation& op)
{
    // Not for -1: the UCRT ends the process on an invalid descriptor.
    HANDLE h = fd >= 0 ? reinterpret_cast<HANDLE>(_get_osfhandle(fd)) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        Throw(op, EBADF, std::generic_category(), "_get_osfhandle");
    }
    if (fd >= static_cast<int>(Files_.size())) {
        Files_.resize(fd + 1, EFile::Unknown);
    }
    auto& file = Files_[fd];
    if (file == EFile::Unknown) {
        // Fails for a handle opened without FILE_FLAG_OVERLAPPED.
        if (!CreateIoCompletionPort(h, Port_, 0, 0)) {
            file = EFile::Sync;
        } else {
            file = GetFileType(h) == FILE_TYPE_DISK ? EFile::Disk : EFile::Port;
        }
    }
    if (file == EFile::Sync) {
        return nullptr;
    }
    op.File = h;
    if (file == EFile::Disk) {
        // Overlapped I/O does not move the file pointer: take the range when posting, so
        // operations posted together get consecutive ranges, as with read() and write().
        LARGE_INTEGER dist, end;
        dist.QuadPart = size;
        if (!SetFilePointerEx(h, dist, &end, FILE_CURRENT)) {
            Throw(op, GetLastError(), std::generic_category(), "SetFilePointerEx");
        }
        ULONGLONG start = end.QuadPart - size;
        op.Offset = static_cast<DWORD>(start);
        op.OffsetHigh = static_cast<DWORD>(start >> 32);
        op.Size = size;
    }
    return h;
}

void TIOCp::PostedFile(TOperation& op, BOOL ok, const char* what)
{
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    // A packet comes unless the call failed: a part of a pipe message is not a failure.
    if (ok || err == ERROR_IO_PENDING || err == ERROR_MORE_DATA) {
        InFlight_++;
        return;
    }
    Rewind(op, 0);
    if (!op.Read || (err != ERROR_HANDLE_EOF && err != ERROR_BROKEN_PIPE)) {
        Throw(op, err, std::generic_category(), what);
    }
    Completions_.Reap(&op, 0);
}

void TIOCp::Rewind(TOperation& op, DWORD transferred)
{
    // Not after the poller closed the descriptor: the handle may be reused.
    if (transferred >= op.Size || Files_[op.Fd] != EFile::Disk) {
        return;
    }
    ULONGLONG start = op.Offset | (static_cast<ULONGLONG>(op.OffsetHigh) << 32);
    LARGE_INTEGER pointer, size;
    LARGE_INTEGER zero = {};
    // Not if a later operation took the pointer or it was moved.
    if (!SetFilePointerEx(op.File, zero, &pointer, FILE_CURRENT) || static_cast<ULONGLONG>(pointer.QuadPart) != start + op.Size) {
        return;
    }
    pointer.QuadPart = start + transferred;
    // Concurrent reads past the end leave no hole for the next write.
    if (op.Read && GetFileSizeEx(op.File, &size) && size.QuadPart < pointer.QuadPart) {
        pointer = size;
    }
    SetFilePointerEx(op.File, pointer, nullptr, FILE_BEGIN);
}

void TIOCp::Read(int fd, void* buf, int size, TOperation& op)
{
    Prepare(op, fd);
    op.Read = true;
    HANDLE h = StartFile(fd, size, op);
    if (!h) {
        // Blocks, as a regular file on the readiness pollers.
        auto ret = TFileOps::read(fd, buf, size);
        Completions_.Reap(&op, ret >= 0 ? ret : -errno);
        return;
    }
    PostedFile(op, ReadFile(h, buf, size, nullptr, static_cast<OVERLAPPED*>(&op)), "ReadFile");
}

void TIOCp::Write(int fd, const void* buf, int size, TOperation& op)
{
    Prepare(op, fd);
    HANDLE h = StartFile(fd, size, op);
    if (!h) {
        auto ret = TFileOps::write(fd, buf, size);
        Completions_.Reap(&op, ret >= 0 ? ret : -errno);
        return;
    }
    PostedFile(op, WriteFile(h, buf, size, nullptr, static_cast<OVERLAPPED*>(&op)), "WriteFile");
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

    CancelIoEx(op.File ? op.File : (HANDLE)(SOCKET)op.Fd, &op);
    // The system may use the operation's buffers until it completes, and its packet points
    // to op, so wait until the packet is dequeued. Other completions are kept for the next Poll().
    bool done = false;
    while (!done) {
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
    if (op->Size) {
        Rewind(*op, entry.dwNumberOfBytesTransferred);
    }
    int result = -1;
    if (!op->Handle) {
        // Cancelled: nobody takes the socket, accepted or not.
        if (op->Addr) {
            closesocket((SOCKET)op->Sock);
        }
    } else if (op->Addr && Failed(*op)) {
        closesocket((SOCKET)op->Sock); // nothing was accepted into it
        result = -SocketError(*op);
    } else if (op->Addr) {
        sockaddr_in* remoteAddr = nullptr;
        sockaddr_in* localAddr = nullptr;
        int localAddrLen = 0;
        *op->Len = 0;
        GetAcceptExSockaddrs(op->Addr, 0, sizeof(sockaddr_in6) + 16, sizeof(sockaddr_in6) + 16, (sockaddr**)&localAddr, &localAddrLen, (sockaddr**)&remoteAddr, op->Len);
        memmove(op->Addr, remoteAddr, *op->Len);
        // Without the listener's context getpeername(), getsockname() and shutdown() fail on the socket.
        SOCKET listener = (SOCKET)op->Fd;
        if (setsockopt((SOCKET)op->Sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char*>(&listener), sizeof(listener)) != 0) {
            // The listener was closed after AcceptEx completed: report it as the abort, like a pending accept.
            int error = WSAGetLastError();
            result = -(error == WSAENOTSOCK ? WSA_OPERATION_ABORTED : error);
            closesocket((SOCKET)op->Sock);
        } else {
            result = op->Sock;
        }
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
    } else if (op->File) {
        // Maps the status to a Win32 error. The operation is complete, so the handle, which
        // may be closed already, is not used.
        DWORD transferred = 0;
        DWORD err = GetOverlappedResult(op->File, op, &transferred, FALSE) ? ERROR_SUCCESS : GetLastError();
        if (err == ERROR_SUCCESS || err == ERROR_MORE_DATA) { // the next read gets the rest of the message
            result = transferred;
        } else if (op->Read && (err == ERROR_HANDLE_EOF || err == ERROR_BROKEN_PIPE)) {
            result = 0;
        } else {
            result = -static_cast<int>(err);
        }
    } else if (Failed(*op)) {
        result = -SocketError(*op);
    } else {
        result = entry.dwNumberOfBytesTransferred;
    }
    return result;
}

void TIOCp::Poll()
{
    for (auto& change : Changes_) {
        if (change.Type & TEvent::CLOSE) {
            if ((change.Type & TEvent::CLOSE_FILE) && change.Fd < static_cast<int>(Files_.size())) {
                Files_[change.Fd] = EFile::Unknown; // a new file may get the number
            }
            // Closing ends the pending overlapped operations, whose packets still reach the port.
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
