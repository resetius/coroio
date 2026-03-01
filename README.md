# COROIO

<img src="/bench/logo.png?raw=true" width="400"/>

C++23 coroutine-based async I/O library. Single-threaded event loop, zero virtual dispatch on the hot path, pluggable polling backends.

[![Stars](https://img.shields.io/github/stars/resetius/coroio)](https://github.com/resetius/coroio/stargazers)
![Commits](https://img.shields.io/github/commit-activity/m/resetius/coroio)
[![License](https://img.shields.io/github/license/resetius/coroio)](https://github.com/resetius/coroio)

---

## Architecture

```
TLoop<TPoller>
  └── TPoller                    ← TEPoll | TKqueue | TIOCp | TUring | TPoll | TSelect
        ├── TSocket              ← async TCP/UDP socket
        ├── TFileHandle          ← async file descriptor
        └── TPollerBase          ← timers, Sleep(), Yield(), AddTimer()
```

`TDefaultPoller` resolves to `TEPoll` on Linux, `TKqueue` on macOS/FreeBSD, `TIOCp` on Windows.

All networking types are obtained as `TPoller::TSocket` / `TPoller::TFileHandle` so they bind to the correct backend at compile time.

### I/O primitives

| Type | Description |
|---|---|
| `TSocket` | Async network socket. `Connect`, `Accept`, `Bind`, `Listen`, `ReadSome`, `WriteSome` |
| `TFileHandle` | Async file descriptor (including stdin/stdout). Same `ReadSome`/`WriteSome` interface |
| `TByteReader(sock)` | Reads exactly N bytes; `ReadUntil(delim)` for delimiter-terminated reads |
| `TByteWriter(sock)` | Writes exactly N bytes |
| `TLineReader(fd, maxSize)` | Line-by-line from a circular buffer → `TLine{Part1, Part2}` |

`ReadSome`/`WriteSome` return bytes transferred, `0` on close, `-1` on transient error.
`TLine` has two parts because the internal circular buffer may wrap around.

---

## Coroutine types

```
TVoidTask           fire-and-forget   cannot be co_await-ed, self-destructs, swallows exceptions
TFuture<void>       owned task        can be co_await-ed, propagates exceptions, RAII handle lifetime
TFuture<T>          owned task        same + carries a return value of type T
```

**`TVoidTask`** — detached coroutine. The caller does not own it; it runs independently and destroys itself on completion. Exceptions are silently dropped. Use for spawned handlers that outlive their creator (e.g. per-connection tasks in an accept loop).

**`TFuture<T>`** — owned coroutine. The caller holds the handle. `co_await future` suspends the caller until the coroutine finishes, then returns `T` (or rethrows a stored exception). `future.done()` polls completion without suspending. The handle is destroyed in `~TFuture`.

```cpp
// TVoidTask: spawn and forget — no co_await possible
TVoidTask handle_connection(TSocket sock) {
    char buf[4096]; ssize_t n;
    while ((n = co_await sock.ReadSome(buf, sizeof(buf))) > 0)
        co_await sock.WriteSome(buf, n);
    // exceptions here are swallowed
}

// TFuture<T>: await the result
TFuture<int> read_int(TSocket& sock) {
    int v; co_await TByteReader(sock).Read(&v, sizeof(v));
    co_return v;
}

TFuture<void> use_it(TSocket& sock) {
    int v = co_await read_int(sock);   // suspends until read_int finishes
    // exception from read_int propagates here
}
```

**Running a `TFuture` from `main`** — drive the loop until `.done()`:

```cpp
int main() {
    NNet::TInitializer init;
    TLoop<TDefaultPoller> loop;
    TFuture<void> task = my_coroutine(loop.Poller(), ...);
    while (!task.done())
        loop.Step();
}
```

**Running a `TVoidTask` from `main`** — the task is detached, so drive the loop by another condition:

```cpp
int main() {
    NNet::TInitializer init;
    TLoop<TDefaultPoller> loop;
    server(loop.Poller(), TAddress{"::", 8888});  // returns TVoidTask, immediately detached
    loop.Loop();                                   // runs forever (until loop.Stop())
}
```

### Combining futures

```cpp
// Wait for all — sequentially awaits each future in order
TFuture<std::vector<int>> f = All(std::move(futures));   // TFuture<vector<T>>
TFuture<void>             f = All(std::move(void_futures));

// Wait for any — resumes when the first one finishes
TFuture<int>  f = Any(std::move(futures));
TFuture<void> f = Any(std::move(void_futures));

// Transform a result without co_await at call site
TFuture<std::string> f = read_int(sock).Apply([](int v) { return std::to_string(v); });

// Discard the return value
TFuture<void> f = read_int(sock).Ignore();

// Run a continuation after a void future
TFuture<void> f = wait_for_event().Accept([] { cleanup(); });
```

---

## Usage

Include `<coroio/all.hpp>`.

### Echo server

```cpp
#include <coroio/all.hpp>
using namespace NNet;

template<typename TSocket>
TVoidTask client_handler(TSocket socket) {
    char buf[4096]; ssize_t n;
    while ((n = co_await socket.ReadSome(buf, sizeof(buf))) > 0)
        co_await socket.WriteSome(buf, n);
}

template<typename TPoller>
TVoidTask server(TPoller& poller, TAddress addr) {
    typename TPoller::TSocket sock(poller, addr.Domain());
    sock.Bind(addr); sock.Listen();
    while (true)
        client_handler(co_await sock.Accept());
}

int main() {
    NNet::TInitializer init;
    TLoop<TDefaultPoller> loop;
    server(loop.Poller(), TAddress{"::", 8888});
    loop.Loop();
}
```

### Echo client (stdin → server → stdout)

```cpp
template<typename TPoller>
TFuture<void> client(TPoller& poller, TAddress addr) {
    using TSocket     = typename TPoller::TSocket;
    using TFileHandle = typename TPoller::TFileHandle;
    char in[4096];

    TFileHandle input{0, poller};          // stdin
    TSocket socket{poller, addr.Domain()};
    TLineReader  lineReader(input, 4096);
    TByteWriter  byteWriter(socket);
    TByteReader  byteReader(socket);

    co_await socket.Connect(addr, TClock::now() + std::chrono::seconds(1));
    while (auto line = co_await lineReader.Read()) {
        co_await byteWriter.Write(line);
        co_await byteReader.Read(in, line.Size());
        std::cout << std::string_view(in, line.Size());
    }
}
```

### Selecting a backend explicitly

```cpp
TLoop<TEPoll>  loop;   // Linux epoll
TLoop<TUring>  loop;   // Linux io_uring
TLoop<TKqueue> loop;   // macOS / FreeBSD
TLoop<TIOCp>   loop;   // Windows IOCP
TLoop<TSelect> loop;   // portable fallback
```

---

## Utilities

### DNS (`<coroio/dns/resolver.hpp>`)

```
TResolvConf          reads /etc/resolv.conf (or any istream) → .Nameservers
TResolver(poller)    async DNS over UDP; caches results; retries on timeout
THostPort(host,port) resolves hostname or passes through a literal IP
```

```cpp
TResolver resolver(loop.Poller());                          // uses /etc/resolv.conf

auto addrs = co_await resolver.Resolve("example.com");      // A record (IPv4)
auto addrs = co_await resolver.Resolve("example.com", EDNSType::AAAA); // IPv6

// THostPort skips DNS for literal IPs
TAddress addr = co_await THostPort("example.com", 80).Resolve(resolver);
```

`TResolver` lives for the duration of the loop and handles multiple concurrent requests; call `Resolve` from multiple coroutines simultaneously.

---

### HTTP server (`<coroio/http/httpd.hpp>`)

```
TWebServer<TSocket>          accept loop + per-connection handler
IRouter                      implement HandleRequest(TRequest&, TResponse&)
TRequest                     method, URI, headers, body (Content-Length or chunked)
TResponse                    SetStatus, SetHeader, SendHeaders, WriteBodyFull / WriteBodyChunk
TUri                         path, query parameters, fragment
```

```cpp
struct MyRouter : NNet::IRouter {
    TFuture<void> HandleRequest(TRequest& req, TResponse& res) override {
        res.SetStatus(200);
        res.SetHeader("Content-Type", "text/plain");
        co_await res.SendHeaders();
        co_await res.WriteBodyFull("hello");
        // or streaming: co_await res.WriteBodyChunk(data, size);
    }
};

// startup
using TSocket = TDefaultPoller::TSocket;
TLoop<TDefaultPoller> loop;
TSocket sock(loop.Poller(), addr.Domain());
sock.Bind(addr); sock.Listen();
MyRouter router;
TWebServer<TSocket> server(std::move(sock), router);
server.Start();
loop.Loop();
```

Reading the request body:
```cpp
std::string body = co_await req.ReadBodyFull();        // slurp entire body
ssize_t n = co_await req.ReadBodySome(buf, size);      // streaming
```

---

### WebSocket (`<coroio/ws/ws.hpp>`)

`TWebSocket<TSocket>` wraps any connected socket with the WebSocket framing layer. Client side only (server upgrade not included). Text frames only.

```cpp
typename TPoller::TSocket sock(poller, addr.Domain());
co_await sock.Connect(addr);

TWebSocket ws(sock);
co_await ws.Connect("example.com", "/chat");   // HTTP upgrade handshake

co_await ws.SendText("hello");
std::string_view msg = co_await ws.ReceiveText();
```

`TWebSocket` holds a reference to the socket — the socket must outlive it.

---

### SSL/TLS (`<coroio/ssl.hpp>`, requires OpenSSL)

```
TSslContext::Client(logFn)                    client context
TSslContext::Server(certfile, keyfile, logFn) server context (PEM files)
TSslContext::ServerFromMem(cert*, key*, logFn) server context (in-memory PEM)
TSslSocket<TSocket>(socket, ctx)              TLS layer; same ReadSome/WriteSome interface
```

```cpp
// TLS client
TSslContext ctx = TSslContext::Client();
TSslSocket ssl(std::move(socket), ctx);
ssl.SslSetTlsExtHostName("example.com");  // SNI
co_await ssl.Connect(addr);
ssize_t n = co_await ssl.ReadSome(buf, size);

// TLS server
TSslContext ctx = TSslContext::Server("server.crt", "server.key");
TSslSocket ssl(std::move(accepted_socket), ctx);
co_await ssl.AcceptHandshake();
```

`TSslSocket` is detected by presence of `<openssl/bio.h>` at compile time (`HAVE_OPENSSL`). `TWebSocket` over TLS works by wrapping `TSslSocket` instead of a plain socket.

---

### Pipe (`<coroio/pipe/pipe.hpp>`, Linux/macOS only)

Spawns a child process and exposes its stdin/stdout/stderr as async handles.

```cpp
TPipe pipe(poller, "/bin/cat", {});           // exe + args
TPipe pipe(poller, "/bin/bash", {"-c", "..."}, /*stderrToStdout=*/true);

co_await pipe.WriteSome(data, size);          // write to child stdin
ssize_t n = co_await pipe.ReadSome(buf, sz); // read child stdout
ssize_t n = co_await pipe.ReadSomeErr(buf, sz); // read child stderr (if not merged)

pipe.CloseWrite();   // send EOF to child
int exit_code = pipe.Wait();
int pid = pipe.Pid();
```

---

## Actor System

Full docs: [Actors on top of coroio](coroio/actors/README.md)

```
TActorSystem
  ├── Register(unique_ptr<IActor>) → TActorId
  ├── Send<T>(to, args...)
  └── AddNode(nodeId, unique_ptr<INode>)  ← distributed; messages go over network
```

Each actor runs on one thread — no synchronization needed. Messages to remote nodes are serialized and sent asynchronously.

**Two actor styles:**

`IActor` — manual dispatch:
```cpp
void Receive(TMessageId id, TBlob blob, TActorContext::TPtr ctx) override {
    if (id == TMyMsg::MessageId) {
        auto msg = DeserializeNear<TMyMsg>(blob);
        ctx->Send<TReply>(ctx->Sender(), ...);
    }
}
```

`IBehaviorActor + TBehavior<Derived, Msg1, Msg2, ...>` — typed dispatch, switchable behavior:
```cpp
struct MyActor : IBehaviorActor, TBehavior<MyActor, TPing, TMessage> {
    MyActor() { Become(this); }

    void Receive(TPing&&, TBlob, TActorContext::TPtr ctx) {
        ctx->Schedule<TPing>(steady_clock::now() + 1s, ctx->Self(), ctx->Self());
    }
    void Receive(TMessage&& msg, TBlob, TActorContext::TPtr ctx) { /* ... */ }
};
```

**`TActorContext` API:**

| Method | Description |
|---|---|
| `ctx->Send<T>(to, args...)` | fire-and-forget |
| `ctx->Forward<T>(to, args...)` | forward, preserving sender |
| `ctx->Schedule<T>(when, from, to, args...)` | delayed send |
| `ctx->Cancel(event)` | cancel a scheduled message |
| `ctx->Ask<T>(to, question)` | request-reply, returns future |
| `ctx->Sleep(duration)` | coroutine sleep |
| `TPoison` | terminates the receiving actor |

**POD messages** serialize automatically. Non-POD types need:
```cpp
template<> void SerializeToStream<T>(const T&, std::ostringstream&);
template<> void DeserializeFromStream<T>(T&, std::istringstream&);
```

---

## Benchmark

Methodology from [libevent](https://libevent.org). Two tests: (1) one active connection, (2) 100 chained connections doing 1000 writes each.

**i7-12800H · Ubuntu 23.04 · clang 16 · libevent 4c993a0**

<img src="/bench/bench_12800H.png?raw=true" width="400"/><img src="/bench/bench_12800H_100.png?raw=true" width="400"/>

**i5-11400F · Ubuntu 23.04 · WSL2 · kernel 6.1.21.1-microsoft-standard-WSL2+**

<img src="/bench/bench_11400F.png?raw=true" width="400"/><img src="/bench/bench_11400F_100.png?raw=true" width="400"/>

**Apple M1 · MacBook Air 16G · macOS 12.6.3**

<img src="/bench/bench_M1.png?raw=true" width="400"/><img src="/bench/bench_M1_100.png?raw=true" width="400"/>

## Actor Benchmark

Ring topology: N actors forwarding a message around the ring M times. Seed messages not counted.

**i5-11400F · Ubuntu 25.04**

Local ring (100 actors, batch 1024):

| Framework | msg/s |
|---|---|
| Akka | 473,966 |
| **Coroio** | **442,151** |
| Caf | 302,930 |
| Ydb/actors | 151,972 |

Distributed ring (10 processes, batch 1024, payload 0 bytes):

| Framework | msg/s |
|---|---|
| **Coroio** | **1,137,790** |
| Ydb/actors | 182,525 |
| Caf | 55,540 |
| Akka | 5,765 |

Distributed ring (10 processes, batch 1024, payload 1024 bytes):

| Framework | msg/s |
|---|---|
| **Coroio** | **860,188** |
| Ydb/actors | 96,372 |

---

## Projects Using coroio

- **miniraft-cpp** — Raft consensus algorithm. [GitHub](https://github.com/resetius/miniraft-cpp)
- **qumir** — Experimental language with Russian keywords; coroio as web server for Playground. [GitHub](https://github.com/resetius/qumir)

[Official Site](https://coroio.dev/)
