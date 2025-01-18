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

/**
 * @class TByteReader
 * @brief A utility for reading data from a socket-like object, either a fixed number of bytes
 *        or until a specified delimiter.
 *
 * This class manages an internal buffer so that any data read beyond what is immediately
 * requested can be stored and used in subsequent reads.
 *
 * @tparam TSocket The socket type used for reading bytes. It must provide a method
 *         <tt>TFuture<ssize_t> ReadSome(void* buffer, size_t size)</tt> which returns:
 *         - 0 on connection closure,
 *         - a positive number for the count of bytes successfully read,
 *         - a negative number to indicate a recoverable read error (in which case a retry may be attempted).
 *
 * ### Example Usage
 * @code{.cpp}
 * TFuture<void> ExampleFunction(TSocket& socket) {
 *     TByteReader<TSocket> reader(socket);
 *
 *     // Read a fixed number of bytes
 *     char data[128];
 *     co_await reader.Read(data, sizeof(data));
 *     // 'data' now contains 128 bytes read from the socket (or an exception is thrown on closure).
 *
 *     // Read until a specific delimiter, e.g. "\r\n"
 *     std::string line = co_await reader.ReadUntil("\r\n");
 *     // 'line' includes the "\r\n" delimiter at the end.
 *
 *     co_return;
 * }
 * @endcode
 */
template<typename TSocket>
struct TByteReader {
    /**
     * @brief Constructs a reader for the given socket.
     * @param socket Reference to a socket-like object used for reading.
     */
    TByteReader(TSocket& socket)
        : Socket(socket)
    { }
    /**
     * @brief Reads exactly @p size bytes and stores them into @p data.
     *
     * - First uses any leftover bytes in the internal buffer.
     * - If more bytes are needed, repeatedly calls the socket's ReadSome()
     *   until the requested amount is fulfilled.
     * - Throws a std::runtime_error if the socket is closed (ReadSome returns 0)
     *   before all requested bytes are read.
     * - If ReadSome returns a negative value, the read is retried.
     *
     * @param data Pointer to the buffer where bytes will be stored.
     * @param size The exact number of bytes to read.
     *
     * @return A TFuture<void> that completes when all bytes are read.
     *
     * @throws std::runtime_error If the connection is closed before @p size bytes are fully read.
     */
    TFuture<void> Read(void* data, size_t size) {
        char* p = static_cast<char*>(data);

        if (!Buffer.empty()) {
            size_t toCopy = std::min(size, Buffer.size());
            std::memcpy(p, Buffer.data(), toCopy);
            p += toCopy;
            size -= toCopy;
            Buffer.erase(0, toCopy);
        }

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
    /**
     * @brief Reads data until the given @p delimiter is encountered.
     *
     * - Searches the internal buffer for the delimiter.
     * - If found, returns all data up to and including the delimiter.
     * - If not found, appends the buffered data to the result and tries reading
     *   more data from the socket, repeating until the delimiter appears.
     * - Throws a std::runtime_error if the socket is closed (ReadSome returns 0)
     *   before the delimiter is found.
     * - If ReadSome returns a negative value, the read is retried.
     *
     * @param delimiter The string to look for in the incoming data.
     * @return A TFuture<std::string> that completes once the delimiter is found,
     *         returning everything up to and including that delimiter.
     *
     * @throws std::runtime_error If the connection is closed before the delimiter is found.
     */
    TFuture<std::string> ReadUntil(const std::string& delimiter)
    {
        std::string result;
        char tempBuffer[1024];

        while (true) {
            auto pos = std::search(Buffer.begin(), Buffer.end(), delimiter.begin(), delimiter.end());
            if (pos != Buffer.end()) {
                size_t delimiterOffset = std::distance(Buffer.begin(), pos);
                result.append(Buffer.substr(0, delimiterOffset + delimiter.size()));
                Buffer.erase(0, delimiterOffset + delimiter.size());
                co_return result;
            }

            result.append(Buffer);
            Buffer.clear();

            auto readSize = co_await Socket.ReadSome(tempBuffer, sizeof(tempBuffer));
            if (readSize == 0) {
                throw std::runtime_error("Connection closed");
            }
            if (readSize < 0) {
                continue; // retry
            }

            Buffer.append(tempBuffer, readSize);
        }

        co_return result;
    }

private:
    TSocket& Socket;
    std::string Buffer;
};

/**
 * @class TByteWriter
 * @brief A utility for writing data to a socket-like object.
 *
 * This class provides methods to write a specified number of bytes or to write
 * a @c TLine object. If the underlying socket indicates it cannot make progress,
 * the write operation is retried; if the socket is closed (write returns 0), an
 * exception is thrown.
 *
 * @tparam TSocket The socket type used for writing bytes. It must provide a method
 *         <tt>TValueTask<ssize_t> WriteSome(const void* data, size_t size)</tt> which returns:
 *         - 0 if the connection is closed,
 *         - a positive number for the count of bytes successfully written,
 *         - a negative number to indicate a recoverable write error (in which case a retry may be attempted).
 *
 * ### Example Usage
 * @code{.cpp}
 * TValueTask<void> ExampleFunction(TSocket& socket) {
 *     TByteWriter<TSocket> writer(socket);
 *
 *     // Write a fixed amount of data
 *     const char* message = "Hello, World!";
 *     co_await writer.Write(message, std::strlen(message));
 *
 *     // Write a TLine object
 *     TLine line{"PartA", "PartB"};
 *     co_await writer.Write(line);
 *
 *     co_return;
 * }
 * @endcode
 */
template<typename TSocket>
struct TByteWriter {
    /**
     * @brief Constructs a writer for the given socket.
     * @param socket Reference to a socket-like object used for writing.
     */
    TByteWriter(TSocket& socket)
        : Socket(socket)
    { }
    /**
     * @brief Writes exactly @p size bytes from @p data to the socket.
     *
     * - Calls the socket's WriteSome() in a loop until all requested bytes are written.
     * - If WriteSome() returns 0, a std::runtime_error is thrown to indicate closure.
     * - If WriteSome() returns a negative value, the write is retried.
     *
     * @param data Pointer to the buffer containing the bytes to write.
     * @param size The number of bytes to write.
     *
     * @return A TValueTask<void> that completes when all bytes have been written
     *         or an exception is thrown if the socket is closed.
     *
     * @throws std::runtime_error If the connection is closed before @p size bytes are fully written.
     */
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
    /**
     * @brief Writes a @c TLine object by sequentially writing its parts.
     *
     * This method calls @ref Write(const void*, size_t) for @c line.Part1
     * and then @c line.Part2.
     *
     * @param line A structure holding the data to write in two parts.
     *
     * @return A TValueTask<void> that completes once both parts have been fully written.
     *
     * @throws std::runtime_error If the connection is closed while writing.
     */
    TValueTask<void> Write(const TLine& line) {
        co_await Write(line.Part1.data(), line.Part1.size());
        co_await Write(line.Part2.data(), line.Part2.size());
        co_return;
    }

private:
    TSocket& Socket;
};

/**
 * @class TStructReader
 * @brief A utility for reading a fixed-size structure of type @p T from a socket-like object.
 *
 * This class expects the socket to provide a method <tt>TValueTask<ssize_t> ReadSome(void* buffer, size_t size)</tt>,
 * which returns:
 * - 0 if the connection is closed,
 * - a positive number for the count of bytes successfully read,
 * - a negative number to indicate a recoverable read error (in which case a retry may be attempted).
 *
 * @tparam T A trivially copyable (or otherwise byte-serializable) type
 *           that can be read directly into memory.
 * @tparam TSocket The socket type used for reading, offering the required
 *                 <tt>ReadSome()</tt> method.
 *
 * ### Example Usage
 * @code{.cpp}
 * // A simple struct to demonstrate reading a fixed-size block of data:
 * struct MyData {
 *     int Id;
 *     float Value;
 * };
 *
 * TValueTask<void> ExampleFunction(TSocket& socket) {
 *     // Create a reader for MyData structures
 *     TStructReader<MyData, TSocket> reader(socket);
 *
 *     // Read one instance of MyData
 *     MyData data = co_await reader.Read();
 *
 *     // 'data' is now populated with the bytes read from the socket
 *     // (assuming the full size of MyData was successfully read).
 *
 *     co_return;
 * }
 * @endcode
 */
template<typename T, typename TSocket>
struct TStructReader {
    /**
     * @brief Constructs a reader with a reference to the given socket.
     * @param socket Reference to a socket-like object used for reading data.
     */
    TStructReader(TSocket& socket)
        : Socket(socket)
    { }
    /**
     * @brief Reads a single instance of type @p T from the socket.
     *
     * - Reads exactly <tt>sizeof(T)</tt> bytes.
     * - Continues to call @c ReadSome() until all needed bytes are read.
     * - Throws a @c std::runtime_error if the socket closes (returns 0)
     *   before the structure is fully read.
     * - Retries if @c ReadSome() returns a negative number (indicating a
     *   recoverable read error).
     *
     * @return A TValueTask<T> that completes once the entire structure is read.
     *
     * @throws std::runtime_error If the connection is closed before the
     *                            structure is fully read.
     */
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
    { }

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
