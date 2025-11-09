#ifndef _WIN32

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <string>

#include "pipe.hpp"

namespace NNet {

namespace {

void SetCloseOnExec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        throw std::runtime_error("fcntl(F_GETFD) failed");
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::runtime_error("fcntl(F_SETFD) failed");
    }
}

void SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        throw std::runtime_error("fcntl(F_GETFL) failed");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl(F_SETFL) failed");
    }
}

} // namespace {

TPipe::TPipeLow::TPipeLow(const std::string& exe, const std::vector<std::string>& args)
    : Exe(exe)
    , Args(args)
{ }

TPipe::TPipeLow::~TPipeLow() {
    if (ChildPid != -1) {
        int status = 0;
        kill(ChildPid, SIGKILL);
        waitpid(ChildPid, &status, 0);
        close(ReadFd);
        close(WriteFd);
    }
}

void TPipe::TPipeLow::Fork() {
    // fork + dup2 + execve
    // exe: Exe
    // args: Args, Arg[0] = Exe

    int pipeStdin[2];
    int pipeStdout[2];
    if (pipe(pipeStdin) == -1) {
        throw std::system_error(errno, std::generic_category(), "pipe() failed for stdin");
    }
    if (pipe(pipeStdout) == -1) {
        auto code = errno;
        close(pipeStdin[0]);
        close(pipeStdin[1]);
        throw std::system_error(code, std::generic_category(), "pipe() failed for stdout");
    }
    int pipeStderr[2];
    if (pipe(pipeStderr) == -1) {
        auto code = errno;
        close(pipeStdin[0]);
        close(pipeStdin[1]);
        close(pipeStdout[0]);
        close(pipeStdout[1]);
        throw std::system_error(code, std::generic_category(), "pipe() failed for stderr");
    }
    SetCloseOnExec(pipeStdin[0]); SetCloseOnExec(pipeStdin[1]);
    SetCloseOnExec(pipeStdout[0]); SetCloseOnExec(pipeStdout[1]);
    SetCloseOnExec(pipeStderr[0]); SetCloseOnExec(pipeStderr[1]);

    auto pid = fork();
    if (pid == -1) {
        auto code = errno;
        close(pipeStdin[0]);
        close(pipeStdin[1]);
        close(pipeStdout[0]);
        close(pipeStdout[1]);
        close(pipeStderr[0]);
        close(pipeStderr[1]);
        throw std::system_error(code, std::generic_category(), "fork() failed");
    }

    if (pid == 0) {
        // Child process
        close(pipeStdin[1]);
        close(pipeStdout[0]);
        close(pipeStderr[0]);

        if (dup2(pipeStdin[0], STDIN_FILENO) == -1) {
            std::cerr << "dup2() failed for stdin: " << strerror(errno) << std::endl;
            _exit(1);
        }
        if (dup2(pipeStdout[1], STDOUT_FILENO) == -1) {
            std::cerr << "dup2() failed for stdout: " << strerror(errno) << std::endl;
            _exit(1);
        }
        if (dup2(pipeStderr[1], STDERR_FILENO) == -1) {
            std::cerr << "dup2() failed for stderr: " << strerror(errno) << std::endl;
            _exit(1);
        }

        close(pipeStdin[0]);
        close(pipeStdout[1]);
        close(pipeStderr[1]);

        std::vector<char*> cargs;
        cargs.reserve(Args.size() + 2);
        cargs.push_back(const_cast<char*>(Exe.c_str()));
        for (const auto& arg : Args) {
            cargs.push_back(const_cast<char*>(arg.c_str()));
        }
        cargs.push_back(nullptr);

        execv(Exe.c_str(), cargs.data());
        std::cerr << "execv() failed: " << strerror(errno) << std::endl;
        _exit(1);
    } else {
        // Parent process
        close(pipeStdin[0]);
        close(pipeStdout[1]);
        close(pipeStderr[1]);

        ChildPid = pid;
        WriteFd = pipeStdin[1];
        ReadFd = pipeStdout[0];
        ErrFd = pipeStderr[0];
        SetNonBlocking(ReadFd);
        SetNonBlocking(WriteFd);
        SetNonBlocking(ErrFd);
    }
}

TFuture<ssize_t> TPipe::ReadSome(void* buffer, size_t size) {
    co_return co_await ReadHandle->ReadSome(buffer, size);
}

TFuture<ssize_t> TPipe::ReadSomeErr(void* buffer, size_t size) {
    co_return co_await ErrHandle->ReadSome(buffer, size);
}

TFuture<ssize_t> TPipe::WriteSome(const void* buffer, size_t size) {
    co_return co_await WriteHandle->WriteSome(buffer, size);
}

int TPipe::Wait() {
    int status = 0;
    if (PipeLow.ChildPid == -1) {
        return -1;
    }
    waitpid(PipeLow.ChildPid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

} // namespace NNet {

#endif // _WIN32
