#pragma once

#include <span>
#include "corochain.hpp"

namespace NNet {

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
            auto readSize = co_await Socket.WriteSome(const_cast<char*>(p) /* TODO: cast */, size);
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

struct TLineSplitter {
public:
    TLineSplitter(int maxLen)
        : WPos(0)
        , RPos(0)
        , Size(0)
        , Cap(maxLen * 2)
        , Data(Cap, 0)
        , View(Data)
    { }

    TLine Pop() {
        auto end = View.substr(RPos, Size);
        auto begin = View.substr(0, Size - end.size());

        auto p1 = end.find('\n');
        if (p1 == std::string_view::npos) {
            auto p2 = begin.find('\n');
            if (p2 == std::string_view::npos) {
                return {};
            }

            RPos = p2 + 1;
            Size -= end.size() + p2 + 1;
            return TLine { end, begin.substr(0, p2 + 1) };
        } else {
            RPos += p1 + 1;
            Size -= p1 + 1;
            return TLine { end.substr(0, p1 + 1), {} };
        }
    }

    void Push(const char* buf, size_t size) {
        if (Size + size > Data.size()) {
            throw std::runtime_error("Overflow");
        }

        auto first = std::min(size, Cap - WPos);
        memcpy(&Data[WPos], buf, first);
        memcpy(&Data[0], buf + first, std::max<size_t>(0, size - first));
        WPos = (WPos + size) % Cap;
        Size = Size + size;
    }

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
    TZeroCopyLineSplitter(int maxLen)
        : WPos(0)
        , RPos(0)
        , Size(0)
        , Cap(maxLen * 2)
        , Data(Cap, 0)
        , View(Data)
    { }

    TLine Pop() {
        auto end = View.substr(RPos, Size);
        auto begin = View.substr(0, Size - end.size());

        auto p1 = end.find('\n');
        if (p1 == std::string_view::npos) {
            auto p2 = begin.find('\n');
            if (p2 == std::string_view::npos) {
                return {};
            }

            RPos = p2 + 1;
            Size -= end.size() + p2 + 1;
            return TLine { end, begin.substr(0, p2 + 1) };
        } else {
            RPos += p1 + 1;
            Size -= p1 + 1;
            return TLine { end.substr(0, p1 + 1), {} };
        }
    }

    std::span<char> Acquire(size_t size) {
        size = std::min(size, Cap - Size);
        if (size == 0) {
            throw std::runtime_error("Overflow");
        }
        auto first = std::min(size, Cap - WPos);
        if (first) {
            return {&Data[WPos], first};
        } else {
            return {&Data[0], size};
        }
    }

    void Commit(size_t size) {
        WPos = (WPos + size) % Cap;
        Size += size;
    }

    void Push(const char* p, size_t len) {
        while (len != 0) {
            auto buf = Acquire(len);
            memcpy(buf.data(), p, buf.size());
            Commit(buf.size());
            len -= buf.size();
            p += buf.size();
        }
    }

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
    TLineReader(TSocket& socket, int maxLineSize, int chunkSize)
        : Socket(socket)
        , Splitter(maxLineSize)
        , ChunkSize(chunkSize)
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
