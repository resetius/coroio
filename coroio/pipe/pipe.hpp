#pragma once

#include <coroio/corochain.hpp>

namespace NNet {

#ifndef _WIN32

/**
 * @brief Spawns a child process and exposes its stdin/stdout/stderr as async handles.
 *
 * Linux/macOS only (`#ifndef _WIN32`). The constructor forks immediately; the child
 * runs `exe` with `args`. The parent side owns three async file handles:
 * - `WriteSome` → child's stdin
 * - `ReadSome`  → child's stdout
 * - `ReadSomeErr` → child's stderr (unless `stderrToStdout = true`)
 *
 * Call `CloseWrite()` to send EOF to the child, then `AsyncWait()` to reap it.
 * The Close*() calls take effect in the poller's next iteration, so call the
 * blocking `Wait()` only once the child's output was read to EOF.
 *
 * @code
 * TPipe pipe(poller, "/bin/cat", {});
 * co_await pipe.WriteSome("hello\n", 6);
 * pipe.CloseWrite();
 *
 * char buf[64];
 * ssize_t n = co_await pipe.ReadSome(buf, sizeof(buf));
 * int exit_code = co_await pipe.AsyncWait();
 * @endcode
 */
class TPipe {
public:
    /**
     * @brief Spawns `exe` with `args` and wires up async I/O handles.
     *
     * @param poller        Event-loop poller used for async I/O on the pipe fds.
     * @param exe           Absolute path to the executable.
     * @param args          Arguments passed to the executable (argv[1..]).
     * @param stderrToStdout If `true`, child stderr is redirected into stdout
     *                      (`ReadSomeErr` becomes unavailable).
     */
    template<typename TPoller>
    TPipe(TPoller& poller, const std::string& exe, const std::vector<std::string>& args, bool stderrToStdout = false)
        : PipeLow(exe, args, stderrToStdout)
        , Poller(&poller)
    {
        PipeLow.Fork();

        ReadHandle = std::make_unique<TPipeFileHandle<TPoller>>(PipeLow.ReadFd, poller);
        WriteHandle = std::make_unique<TPipeFileHandle<TPoller>>(PipeLow.WriteFd, poller);
        if (!stderrToStdout) {
            ErrHandle = std::make_unique<TPipeFileHandle<TPoller>>(PipeLow.ErrFd, poller);
        }
    }

    /// Returns the child's process ID.
    int Pid() const { return PipeLow.ChildPid; }

    /// Closes the read end of the child's stdout pipe.
    void CloseRead() {
        ReadHandle.reset();
    }

    /// Closes the write end of the child's stdin pipe, sending EOF to the child.
    void CloseWrite() {
        WriteHandle.reset();
    }

    /// Closes the read end of the child's stderr pipe.
    void CloseErr() {
        ErrHandle.reset();
    }

    /**
     * @brief Waits for the child process to exit and returns its exit status.
     *
     * Blocks the calling thread (via `waitpid`) without running the loop. Call after
     * the child has finished or after sending it a signal; see the class description.
     *
     * Once it returns the child is reaped and `Pid()` returns `-1`.
     *
     * @return The child's exit code, or `-1` if it did not exit normally or was
     *         already reaped.
     */
    int Wait();

    /**
     * @brief Waits for the child process to exit without blocking the event loop.
     *
     * Polls `waitpid(WNOHANG)` and sleeps on the poller between attempts with a
     * backoff growing from 1 ms to 32 ms, so it works with every poller backend.
     * Once it completes the child is reaped and `Pid()` returns `-1`.
     *
     * The returned future refers to this `TPipe`, so the pipe must outlive the
     * future: destroying it while the future is suspended is a use-after-free.
     *
     * @return The child's exit code, or `-1` if it did not exit normally or was
     *         already reaped.
     */
    TFuture<int> AsyncWait();

    /**
     * @brief Reads up to `size` bytes from the child's stdout.
     *
     * Returns bytes read (>0), `0` on EOF (child closed stdout), or a negative
     * retry hint on transient errors.
     */
    TFuture<ssize_t> ReadSome(void* buffer, size_t size);

    /**
     * @brief Reads up to `size` bytes from the child's stderr.
     *
     * Only valid when `stderrToStdout = false`. Returns bytes read (>0), `0` on
     * EOF, or a negative retry hint.
     */
    TFuture<ssize_t> ReadSomeErr(void* buffer, size_t size);

    /**
     * @brief Writes up to `size` bytes to the child's stdin.
     *
     * Returns bytes written (>0), `0` if the child closed stdin, or a negative
     * retry hint on transient errors.
     */
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

    struct ITypelessFileHandle {
        virtual ~ITypelessFileHandle() = default;
        virtual TFuture<ssize_t> ReadSome(void* buffer, size_t size) = 0;
        virtual TFuture<ssize_t> WriteSome(const void* buffer, size_t size) = 0;
    };

    template<typename TPoller>
    struct TPipeFileHandle final : public ITypelessFileHandle {
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
    TPollerBase* Poller;
    std::unique_ptr<ITypelessFileHandle> ReadHandle;
    std::unique_ptr<ITypelessFileHandle> WriteHandle;
    std::unique_ptr<ITypelessFileHandle> ErrHandle;
};

#endif  // _WIN32

} // namespace NNet {
