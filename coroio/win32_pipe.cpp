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
#endif