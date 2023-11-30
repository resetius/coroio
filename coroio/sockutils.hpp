#pragma once

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
            auto read_size = co_await Socket.ReadSome(p, size);
            if (read_size == 0) {
                throw std::runtime_error("Connection closed");
            }
            if (read_size < 0) {
                continue; // retry
            }
            p += read_size;
            size -= read_size;
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
            auto read_size = co_await Socket.WriteSome(const_cast<char*>(p) /* TODO: cast */, size);
            if (read_size == 0) {
                throw std::runtime_error("Connection closed");
            }
            if (read_size < 0) {
                continue; // retry
            }
            p += read_size;
            size -= read_size;
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
            auto read_size = co_await Socket.ReadSome(p, size);
            if (read_size == 0) {
                throw std::runtime_error("Connection closed");
            }
            if (read_size < 0) {
                continue; // retry
            }
            p += read_size;
            size -= read_size;
        }
        co_return res;
    }

private:
    TSocket& Socket;
};

} // namespace NNet {
