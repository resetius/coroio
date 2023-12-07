#pragma once

#include <assert.h>
#include <span>
#include "corochain.hpp"

namespace NNet {

struct TLine {
    std::string_view Part1;
    std::string_view Part2;

    size_t Size() const {
        return Part1.size() + Part2.size();
    }

    operator bool() const {
        return !Part1.empty();
    }
};

template<typename TSocket>
struct TByteReader {
    TByteReader(TSocket& socket)
        : Socket(socket)
    { }

    TValueTask<void> Read(void* data, size_t size) {
        char* p = static_cast<char*>(data);
        while (size != 0) {
            auto readSize = co_await Socket.ReadSome(p, size);
            if (readSize == 0) {
                throw std::runtime_error("Connection closed");
            }
            if (readSize < 0) {
                continue; // retry
            }
            p += readSize;
            size -= readSize;
        }
        co_return;
    }

private:
    TSocket& Socket;
};

template<typename TSocket>
struct TByteWriter {
    TByteWriter(TSocket& socket)
        : Socket(socket)
    { }

    TValueTask<void> Write(const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        while (size != 0) {
            auto readSize = co_await Socket.WriteSome(p, size);
            if (readSize == 0) {
                throw std::runtime_error("Connection closed");
            }
            if (readSize < 0) {
                continue; // retry
            }
            p += readSize;
            size -= readSize;
        }
        co_return;
    }

    TValueTask<void> Write(const TLine& line) {
        co_await Write(line.Part1.data(), line.Part1.size());
        co_await Write(line.Part2.data(), line.Part2.size());
        co_return;
    }

private:
    TSocket& Socket;
};

template<typename T, typename TSocket>
struct TStructReader {
    TStructReader(TSocket& socket)
        : Socket(socket)
    { }

    TValueTask<T> Read() {
        T res;
        size_t size = sizeof(T);
        char* p = reinterpret_cast<char*>(&res);
        while (size != 0) {
            auto readSize = co_await Socket.ReadSome(p, size);
            if (readSize == 0) {
                throw std::runtime_error("Connection closed");
            }
            if (readSize < 0) {
                continue; // retry
            }
            p += readSize;
            size -= readSize;
        }
        co_return res;
    }

private:
    TSocket& Socket;
};

struct TLineSplitter {
public:
    TLineSplitter(int maxLen);

    TLine Pop();
    void Push(const char* buf, size_t size);

private:
    size_t WPos;
    size_t RPos;
    size_t Size;
    size_t Cap;
    std::string Data;
    std::string_view View;
};

struct TZeroCopyLineSplitter {
public:
    TZeroCopyLineSplitter(int maxLen);

    TLine Pop();
    std::span<char> Acquire(size_t size);
    void Commit(size_t size);
    void Push(const char* p, size_t len);

private:
    size_t WPos;
    size_t RPos;
    size_t Size;
    size_t Cap;
    std::string Data;
    std::string_view View;
};

template<typename TSocket>
struct TLineReader {
    TLineReader(TSocket& socket, int maxLineSize = 4096)
        : Socket(socket)
        , Splitter(maxLineSize)
        , ChunkSize(maxLineSize / 2)
    {
        assert(maxLineSize >= chunkSize);
    }

    TValueTask<TLine> Read() {
        auto line = Splitter.Pop();
        while (!line) {
            auto buf = Splitter.Acquire(ChunkSize);
            auto size = co_await Socket.ReadSome(buf.data(), buf.size());
            if (size < 0) {
                continue;
            }
            if (size == 0) {
                break;
            }
            Splitter.Commit(size);
            line = Splitter.Pop();
        }
        co_return line;
    }

private:
    TSocket& Socket;
    TZeroCopyLineSplitter Splitter;
    int ChunkSize;
};

} // namespace NNet {
