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