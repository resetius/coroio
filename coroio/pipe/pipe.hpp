#pragma once

#include <coroio/corochain.hpp>

namespace NNet {

#ifndef _WIN32

class TPipe {
public:
    template<typename TPoller>
    TPipe(TPoller& poller, const std::string& exe, const std::vector<std::string>& args, bool stderrToStdout = false)
        : PipeLow(exe, args, stderrToStdout)
    {
        PipeLow.Fork();

        ReadHandle = std::make_unique<TPipeFileHandle<TPoller>>(PipeLow.ReadFd, poller);
        WriteHandle = std::make_unique<TPipeFileHandle<TPoller>>(PipeLow.WriteFd, poller);
        if (!stderrToStdout) {
            ErrHandle = std::make_unique<TPipeFileHandle<TPoller>>(PipeLow.ErrFd, poller);
        }
    }

    int Pid() const { return PipeLow.ChildPid; }

    void CloseRead() {
        ReadHandle.reset();
    }

    void CloseWrite() {
        WriteHandle.reset();
    }

    void CloseErr() {
        ErrHandle.reset();
    }

    int Wait();

    TFuture<ssize_t> ReadSome(void* buffer, size_t size);
    TFuture<ssize_t> ReadSomeErr(void* buffer, size_t size);
    TFuture<ssize_t> WriteSome(const void* buffer, size_t size);

private:
    struct TPipeLow {
        TPipeLow(const std::string& exe, const std::vector<std::string>& args, bool mergeErr);
        ~TPipeLow();

        void Fork();

        std::string Exe;
        std::vector<std::string> Args;

        // Descriptors are owned by TPipeFileHandle, do not close them!
        int ReadFd = -1;
        int WriteFd = -1;
        int ErrFd = -1;
        int ChildPid = -1;
        bool StderrToStdout = false;
    };

    struct TTypelessFileHandle {
        virtual ~TTypelessFileHandle() = default;
        virtual TFuture<ssize_t> ReadSome(void* buffer, size_t size) = 0;
        virtual TFuture<ssize_t> WriteSome(const void* buffer, size_t size) = 0;
    };

    template<typename TPoller>
    struct TPipeFileHandle : public TTypelessFileHandle {
        TPipeFileHandle(int fd, TPoller& poller)
            : Handle(fd, poller)
        { }

        TFuture<ssize_t> ReadSome(void* buffer, size_t size) override {
            co_return co_await Handle.ReadSome(buffer, size);
        }
        TFuture<ssize_t> WriteSome(const void* buffer, size_t size) override {
            co_return co_await Handle.WriteSome(buffer, size);
        }

        typename TPoller::TFileHandle Handle;
    };

    TPipeLow PipeLow;
    std::unique_ptr<TTypelessFileHandle> ReadHandle;
    std::unique_ptr<TTypelessFileHandle> WriteHandle;
    std::unique_ptr<TTypelessFileHandle> ErrHandle;
};

#endif  // _WIN32

} // namespace NNet {
