#pragma once

#include <assert.h>
#include <span>
#include <algorithm>
#include "corochain.hpp"

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

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
    TFuture<void> Write(const void* data, size_t size) {
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
    TFuture<void> Write(const TLine& line) {
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
    TFuture<T> Read() {
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

/**
 * @class TLineSplitter
 * @brief Splits incoming data into lines using a circular buffer of fixed capacity.
 *
 * This class maintains a ring buffer of maximum length (@p maxLen), allowing you
 * to push new data in and then pop complete lines without additional copying.
 * When a line wraps around the circular boundary, it is represented in two segments.
 * A corresponding @c TLine object holds these segments as string views (@c Part1 and @c Part2).
 *
 * ### Important Notes
 * - The maximum capacity of the buffer is given by @p maxLen. Pushing more data than
 *   the available space may cause old data to be overwritten or an implementation-defined behavior.
 * - The returned @c TLine references the internal buffer via string views.
 *   Any subsequent push operations (or further pops) may invalidate these views.
 *
 * ### Example Usage
 * @code{.cpp}
 * // Suppose TLine is defined as:
 * // struct TLine {
 * //     std::string_view Part1;
 * //     std::string_view Part2; // empty if the line doesn't wrap around
 * // };
 *
 * int main() {
 *     TLineSplitter splitter(1024); // up to 1024 bytes of data stored
 *
 *     // Push some data containing two lines
 *     const char* data = "Hello\nWorld\n";
 *     splitter.Push(data, std::strlen(data));
 *
 *     // Pop the first line -> "Hello"
 *     TLine line1 = splitter.Pop();
 *     // line1.Part1: "Hello"
 *     // line1.Part2: "" (empty, not wrapped)
 *
 *     // Pop the second line -> "World"
 *     TLine line2 = splitter.Pop();
 *     // line2.Part1: "World"
 *     // line2.Part2: "" (empty, not wrapped)
 *
 *     return 0;
 * }
 * @endcode
 */
struct TLineSplitter {
public:
    /**
     * @brief Constructs a line splitter with a fixed ring buffer capacity.
     * @param maxLen The maximum number of bytes the circular buffer can hold.
     */
    TLineSplitter(int maxLen);
    /**
     * @brief Retrieves and removes the next complete line from the buffer.
     *
     * A line is typically delimited by a newline character (implementation-specific).
     * If the line crosses the circular boundary, @c TLine will contain two parts:
     *   - @c Part1 referencing the tail end of the buffer,
     *   - @c Part2 referencing the beginning portion.
     *
     * @return A @c TLine object with string views that reference the extracted line.
     *         If there is no complete line available, behavior may be undefined
     *         or implementation-dependent (e.g., returns an empty line or throws).
     *
     * @warning The returned @c TLine's string views are valid only until the
     *          next call to @ref Push() or further modifications to the buffer.
     */
    TLine Pop();
    /**
     * @brief Appends new data to the circular buffer.
     *
     * The data is copied into the ring buffer without extra allocations. If the
     * buffer does not have enough free space, the runtime_exception is thrown.
     *
     * @param buf  Pointer to the raw bytes to insert.
     * @param size Number of bytes to insert from @p buf.
     */
    void Push(const char* buf, size_t size);

private:
    size_t WPos;
    size_t RPos;
    size_t Size;
    size_t Cap;
    std::string Data;
    std::string_view View;
};

/**
 * @class TZeroCopyLineSplitter
 * @brief Splits incoming data into lines using a fixed-size circular buffer,
 *        enabling zero-copy writes via @ref Acquire() and @ref Commit().
 *
 * This class maintains a ring buffer of maximum length (@p maxLen) where new data
 * can be placed without extra copying. To add data, a typical workflow would be:
 *   1. Call @ref Acquire() to get a @c std::span<char> in the internal buffer.
 *   2. Write directly into that span (e.g., using your socket's @c read or @c ReadSome).
 *   3. Call @ref Commit() with the number of bytes actually written.
 *
 * Lines can then be extracted using @ref Pop(), which returns a @c TLine holding
 * up to two string-view segments if the line crosses the buffer boundary.
 *
 * ### Zero-Copy Example (Reading from a Socket)
 * @code{.cpp}
 * // Example function that reads lines from a socket into TZeroCopyLineSplitter
 * void ReadLinesFromSocket(TSocket& socket) {
 *     TZeroCopyLineSplitter splitter(1024); // ring buffer up to 1024 bytes
 *     while (true) {
 *         // Acquire a chunk of the buffer (say 256 bytes)
 *         auto span = splitter.Acquire(256);
 *         if (span.empty()) {
 *             // Not enough space left - handle as needed (e.g., flush or error)
 *         }
 *
 *         // Read directly into the splitter's internal buffer
 *         ssize_t bytesRead = socket.ReadSome(span.data(), span.size());
 *         if (bytesRead <= 0) {
 *             // 0 => socket closed, negative => error
 *             break;
 *         }
 *
 *         // Commit the data we actually wrote
 *         splitter.Commit(bytesRead);
 *
 *         // Now try popping lines
 *         while (true) {
 *             TLine line = splitter.Pop();
 *             if (line.Part1.empty() && line.Part2.empty()) {
 *                 // No complete line available at the moment
 *                 break;
 *             }
 *             // Process the line (Part1 + Part2)...
 *         }
 *     }
 * }
 * @endcode
 */
struct TZeroCopyLineSplitter {
public:
    /**
     * @brief Constructs a zero-copy line splitter with a fixed ring buffer capacity.
     * @param maxLen The maximum number of bytes the buffer can hold.
     */
    TZeroCopyLineSplitter(int maxLen);
    /**
     * @brief Extracts and removes the next complete line from the buffer, if available.
     *
     * A line is typically delimited by a newline character (implementation-specific).
     * If the line crosses the circular boundary, the returned @c TLine will contain
     * two segments (@c Part1 and @c Part2).
     *
     * @return A @c TLine object with up to two @c std::string_view segments referencing
     *         the internal ring buffer. If there is no complete line available,
     *         behavior is implementation-defined (it may return an empty @c TLine
     *         or throw an exception).
     *
     * @warning The returned string views become invalid once additional data is
     *          acquired, committed, or popped. Keep this in mind if you need to
     *          store the line contents for later use.
     */
    TLine Pop();
    /**
     * @brief Reserves space in the circular buffer for writing data directly
     *        (e.g., from a socket read) without extra copying.
     *
     * This method returns a contiguous block of available space as a @c std::span<char>.
     * If the ring buffer wraps around, you might only get the block up to the end;
     * you can call @ref Acquire() again for any remaining space, depending on your logic.
     *
     * @param size The desired number of bytes to acquire.
     * @return A @c std::span<char> pointing to the ring buffer region where data can be written.
     *         Its size might be less than requested if there's less contiguous space available.
     *
     * @note You must call @ref Commit() after writing into this span, specifying how many
     *       bytes were actually written. Otherwise, the new data won't be recognized
     *       by the splitter.
     */
    std::span<char> Acquire(size_t size);
    /**
     * @brief Finalizes the amount of data written into the span returned by @ref Acquire().
     *
     * @param size The number of bytes that were actually written into the acquired buffer.
     *
     * After calling @c Commit(), this new data is considered part of the buffer and
     * can be used to form lines via @ref Pop().
     */
    void Commit(size_t size);
    /**
     * @brief (Optional) Copies data from an external buffer into the circular buffer.
     *
     * While the main purpose of this class is zero-copy insertion via @ref Acquire()
     * and @ref Commit(), this method offers a fallback for situations where you
     * already have data in a separate buffer and wish to write it into the splitter
     * in one call.
     *
     * @param p   Pointer to the data to copy from.
     * @param len Number of bytes to copy.
     */
    void Push(const char* p, size_t len);

private:
    size_t WPos;
    size_t RPos;
    size_t Size;
    size_t Cap;
    std::string Data;
    std::string_view View;
};
/**
 * @class TLineReader
 * @brief Reads a complete line from a socket using a zero-copy line splitter.
 *
 * This class encapsulates a socket and a zero-copy line splitter to provide
 * efficient line-based reading. Data is read from the socket in chunks and fed
 * into a circular buffer maintained by the splitter. The splitter then extracts
 * complete lines as soon as they become available.
 *
 * The maximum line size can be specified via the constructor (default is 4096 bytes);
 * a chunk size (default to half of the maximum line size) is used when acquiring space
 * from the splitter for new data. Each call to @ref Read() returns a complete line
 * (of type @c TLine). A @c TLine typically contains string views @c Part1 and @c Part2,
 * which together represent the line (useful when a line wraps around the circular buffer).
 *
 * @tparam TSocket The socket type used for reading. TSocket must provide a method:
 *         <tt>TValueTask<ssize_t> ReadSome(void* buffer, size_t size)</tt>
 *         that reads data asynchronously.
 *
 * ### Example Usage
 * @code{.cpp}
 * // Example function that processes lines read from the socket:
 * TValueTask<void> processLines(TSocket& socket) {
 *     TLineReader<TSocket> lineReader(socket);
 *     while (true) {
 *         TLine line = co_await lineReader.Read();
 *         if (!line) {
 *             // No complete line is available (or end-of-stream reached)
 *             break;
 *         }
 *         // Process the line here (line.Part1 and line.Part2 if the line wraps)
 *     }
 *     co_return;
 * }
 * @endcode
 */
template<typename TSocket>
struct TLineReader {
    /**
     * @brief Constructs a line reader with the given socket and maximum line size.
     *
     * The maximum line size (default 4096) determines the capacity of the underlying
     * zero-copy line splitter. The chunk size is set to half the maximum line size.
     *
     * @param socket The socket used for reading data.
     * @param maxLineSize Maximum allowed size for a line. Default is 4096 bytes.
     */
    TLineReader(TSocket& socket, int maxLineSize = 4096)
        : Socket(socket)
        , Splitter(maxLineSize)
        , ChunkSize(maxLineSize / 2)
    { }
    /**
     * @brief Reads and returns the next complete line from the socket.
     *
     * This method repeatedly attempts to pop a complete line from the splitter.
     * If no complete line is available, it acquires a chunk of the splitter’s internal
     * buffer and reads data from the socket into that space. After committing the
     * newly read data, it retries extracting a line. The method returns a @c TLine
     * (which may contain two parts, if the line wraps around the circular buffer).
     *
     * @return A TValueTask<TLine> that completes once a full line is available.
     */
    TFuture<TLine> Read() {
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
