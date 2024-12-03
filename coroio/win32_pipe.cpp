#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>

int pipe(int pipes[2]) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return -1;
    }

    DWORD mode = PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(readPipe, &mode, NULL, NULL)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return -1;
    }

    pipes[0] = _open_osfhandle(reinterpret_cast<intptr_t>(readPipe), _O_RDONLY);
    pipes[1] = _open_osfhandle(reinterpret_cast<intptr_t>(writePipe), _O_WRONLY);

    if (pipes[0] == -1 || pipes[1] == -1) {
        if (pipes[0] != -1) close(pipes[0]);
        if (pipes[1] != -1) close(pipes[1]);
        return -1;
    }

    return 0;
}

int socketpair(int domain, int type, int protocol, SOCKET socks[2]) {
    if (domain != AF_INET || type != SOCK_STREAM || protocol != 0) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }

    SOCKET listener = INVALID_SOCKET;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        return -1;
    }

    int optval = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval)) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    int addrlen = sizeof(addr);
    if (getsockname(listener, (sockaddr*)&addr, &addrlen) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    if (listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    socks[0] = socket(AF_INET, SOCK_STREAM, 0);
    if (socks[0] == INVALID_SOCKET) {
        closesocket(listener);
        return -1;
    }

    if (setsockopt(socks[0], SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval)) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    if (connect(socks[0], (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(socks[0]);
        closesocket(listener);
        return -1;
    }

    socks[1] = accept(listener, NULL, NULL);
    if (socks[1] == INVALID_SOCKET) {
        closesocket(socks[0]);
        closesocket(listener);
        return -1;
    }

    u_long mode = 1;
    if (ioctlsocket(socks[0], FIONBIO, &mode) != 0) {
        closesocket(socks[0]);
        closesocket(socks[1]);
        closesocket(listener);
        return -1;
    }
    if (ioctlsocket(socks[1], FIONBIO, &mode) != 0) {
        closesocket(socks[0]);
        closesocket(socks[1]);
        closesocket(listener);
        return -1;
    }

    struct linger so_linger = {1, 0};
    setsockopt(socks[0], SOL_SOCKET, SO_LINGER, (char*)&so_linger, sizeof(so_linger));
    setsockopt(socks[1], SOL_SOCKET, SO_LINGER, (char*)&so_linger, sizeof(so_linger));

    closesocket(listener);
    return 0;
}

int socketpair(int domain, int type, int protocol, int socks[2]) {
    SOCKET tmp[2];
    int ret = socketpair(domain, type, protocol, &tmp[0]);
    if (ret != 0) {
        return ret;
    }
    socks[0] = tmp[0];
    socks[1] = tmp[1];
    return ret;
}

#endif