# COROIO

<img src="/bench/logo.png?raw=true" width="400"/>

### Guide to Using the Library

#### Overview
This library leverages C++20 coroutines for asynchronous programming, providing efficient and non-blocking I/O operations. It offers a range of polling mechanisms and utilities for handling sockets and files, making it suitable for various networking and file I/O tasks.

 :star: If you find COROIO useful, please consider giving us a star on GitHub! Your support helps us continue to innovate and deliver exciting features.

[![Number of GitHub stars](https://img.shields.io/github/stars/resetius/coroio)](https://github.com/resetius/coroio/stargazers)
![GitHub commit activity](https://img.shields.io/github/commit-activity/m/resetius/coroio)
[![GitHub license which is BSD-2-Clause license](https://img.shields.io/github/license/resetius/coroio)](https://github.com/resetius/coroio)

#### Key Features

1. **Coroutines for Asynchronous Code**:
   - The library uses C++20 coroutines, allowing you to write asynchronous code in a more straightforward and readable manner.

2. **Polling Mechanisms**:
   - **`TSelect`**: Utilizes the `select` system call, suitable for a wide range of platforms.
   - **`TPoll`**: Uses the `poll` system call, offering another general-purpose polling solution.
   - **`TEPoll`**: Employs `epoll`, available exclusively on Linux systems for high-performance I/O.
   - **`TUring`**: Integrates with `liburing` for advanced I/O operations, specific to Linux.
   - **`TKqueue`**: Uses `kqueue`, available on FreeBSD and macOS.
   - **`TIOCp`**: Uses IO completion ports on Windows.
   - **`TDefaultPoll`**: Automatically selects the best polling mechanism based on the platform (TEPoll on Linux, TKqueue on macOS/FreeBSD, TIOCp on Windows).

3. **Socket and File Handling**:
   - **`TSocket`** and **`TFileHandle`**: Core entities for handling network sockets and file operations.
   - Provide `ReadSome` and `WriteSome` methods for reading and writing data. These methods read or write up to a specified number of bytes, returning the number of bytes processed or -1 on error. A return value of 0 indicates a closed socket.

4. **Utility Wrappers**:
   - **`TByteReader`** and **`TByteWriter`**: Ensure the specified number of bytes is read or written, useful for guaranteed data transmission.
   - **`TLineReader`**: Facilitates line-by-line reading, simplifying the handling of text-based protocols or file inputs.

#### Supported Operating Systems

The library supports the following operating systems:

- **Linux**: Fully supported with `epoll` and `liburing` for high-performance I/O operations.
- **FreeBSD**: Supported via the `kqueue` mechanism.
- **macOS**: Supported via the `kqueue` mechanism.
- **Windows**: Supported using the `iocp` mechanism.

#### Using the Library

1. **Setup**: Include the library in your project and ensure C++20 support is enabled in your compiler settings.

2. **Selecting a Poller**:
   - Choose a polling mechanism based on your platform and performance needs. For most cases, `TDefaultPoll` can automatically select the appropriate poller.

3. **Implementing Network Operations**:
   - Use `TSocket` for network communication. Initialize a socket with the desired address and use `ReadSome`/`WriteSome` for data transmission.
   - Employ `TFileHandle` for file I/O operations with similar read/write methods.

4. **Reading and Writing Data**:
   - For basic operations, use `ReadSome` and `WriteSome`.
   - When you need to ensure a specific amount of data is transmitted, use `TByteReader` or `TByteWriter`.
   - For reading text files or protocols, `TLineReader` offers a convenient way to process data line by line.

#### Example

```cpp
// Example of creating a socket and reading/writing data
TSocket socket{/* initialize with poller */};
// Writing data
socket.WriteSome(data, dataSize);
// Reading data
socket.ReadSome(buffer, bufferSize);
```

#### Best Practices

- Choose the right poller for your platform and performance requirements.
- Always check the return values of `ReadSome` and `WriteSome` to handle partial reads/writes and errors appropriately.
- Use the utility wrappers (`TByteReader`, `TByteWriter`, `TLineReader`) to simplify common I/O patterns.

### Echo Client Example Using `TLineReader`, `TByteReader`, and `TByteWriter`

```cpp
#include <coroio/all.hpp>
#include <iostream>
#include <string>
#include <vector>

using namespace NNet;

template<typename TPoller>
TFuture<void> client(TPoller& poller, TAddress addr)
{
    static constexpr int maxLineSize = 4096;
    using TSocket = typename TPoller::TSocket;
    using TFileHandle = typename TPoller::TFileHandle;
    std::vector<char> in(maxLineSize);

    try {
        TFileHandle input{0, poller}; // stdin
        TSocket socket{poller, addr.Domain()};
        TLineReader lineReader(input, maxLineSize);
        TByteWriter byteWriter(socket);
        TByteReader byteReader(socket);

        co_await socket.Connect(addr);
        while (auto line = co_await lineReader.Read()) {
            co_await byteWriter.Write(line);
            co_await byteReader.Read(in.data(), line.Size());
            std::cout << "Received: " << std::string_view(in.data(), line.Size()) << "\n";
        }
    } catch (const std::exception& ex) {
        std::cout << "Exception: " << ex.what() << "\n";
    }

    co_return;
}

int main() {
    // Initialize your poller (e.g., TSelect, TEpoll)
    // ...

    // Run the Echo Client
    // ...
}
```

#### Key Points of the Example

1. **Line Reading**:
   - `TLineReader` is used to read lines from standard input. It handles lines split into two parts (`Part1` and `Part2`) due to the internal use of a fixed-size circular buffer.

2. **Data Writing**:
   - `TByteWriter` is utilized to write the line parts to the socket, ensuring that the entire line is sent to the server.

3. **Data Reading**:
   - `TByteReader` reads the server's response into a buffer, which is then printed to the console.

4. **Socket Connection**:
   - The `TSocket` is connected to the server at "127.0.0.1" on port 8000.

5. **Processing Loop**:
   - The loop continues reading lines from standard input and echoes back the server's response until the input stream ends.

## Benchmark

The benchmark methodology was taken from the [libevent library](https://libevent.org).

There are two benchmarks. The first one measures how long it takes to serve one active connection and exposes scalability issues of traditional interfaces like select or poll. The second benchmark measures how long it takes to serve one hundred active connections that chain writes to new connections until thousand writes and reads have happened. It exercises the event loop several times.

Performance comparison using different event notification mechansims in Libevent and coroio as follows.

* CPU i7-12800H
* Ubuntu 23.04
* clang 16
* libevent master 4c993a0e7bcd47b8a56514fb2958203f39f1d906 (Tue Apr 11 04:44:37 2023 +0000)

<img src="/bench/bench_12800H.png?raw=true" width="400"/><img src="/bench/bench_12800H_100.png?raw=true" width="400"/>


* CPU i5-11400F
* Ubuntu 23.04, WSL2, kernel 6.1.21.1-microsoft-standard-WSL2+

<img src="/bench/bench_11400F.png?raw=true" width="400"/><img src="/bench/bench_11400F_100.png?raw=true" width="400"/>

* CPU Apple M1
* MacBook Air M1 16G
* MacOS 12.6.3

<img src="/bench/bench_M1.png?raw=true" width="400"/><img src="/bench/bench_M1_100.png?raw=true" width="400"/>

### Projects Using coroio

- **miniraft-cpp**: A minimal implementation of the Raft consensus algorithm, leveraging coroio for efficient and asynchronous I/O operations. [View on GitHub](https://github.com/resetius/miniraft-cpp).


### Official Site
[Official Site](https://coroio.dev/)
