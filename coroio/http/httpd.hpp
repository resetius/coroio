#pragma once

#include <coroio/socket.hpp>
#include <coroio/corochain.hpp>
#include <coroio/sockutils.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <cstdlib>

namespace NNet
{

/**
 * @brief Parsed HTTP request URI: `/path?key=value&...#fragment`.
 *
 * Constructed from the raw URI string found in the request line. Provides
 * typed access to the path, query parameters, and fragment.
 */
// /path?arg1=value1&arg2=value2#fragment
class TUri {
public:
    TUri() = default;
    /// Parses `uriStr` into path, query parameters, and fragment.
    TUri(const std::string& uriStr);
    /// Returns the decoded path component (e.g. `"/api/v1/users"`).
    const std::string& Path() const;
    /// Returns query parameters as a key→value map (e.g. `{"page": "2"}`).
    const std::map<std::string, std::string>& QueryParameters() const;
    /// Returns the fragment (the part after `#`), or empty string if absent.
    const std::string& Fragment() const;

private:
    void Parse(const std::string& uriStr);
    std::string Path_;
    std::map<std::string, std::string> QueryParameters_;
    std::string Fragment_;
};

/**
 * @brief Represents an incoming HTTP/1.1 request.
 *
 * Constructed by `TWebServer` from the parsed request headers and a body-reader
 * callback. The body is read lazily — call `ReadBodyFull()` or `ReadBodySome()`
 * to consume it. Both `Content-Length` and chunked transfer encoding are supported.
 *
 * Header string views (`Method()`, `Version()`, values in `Headers()`) reference
 * the internal header buffer and are valid for the lifetime of this object.
 */
class TRequest {
public:
    TRequest(std::string&& header,
        std::function<TFuture<ssize_t>(char*, size_t)> bodyReader,
        std::function<TFuture<std::string>()> chunkHeaderReader = {});
    /// HTTP method string (e.g. `"GET"`, `"POST"`).
    std::string_view Method() const;
    /// Parsed request URI.
    const TUri& Uri() const;
    /// HTTP version string (e.g. `"HTTP/1.1"`).
    std::string_view Version() const { return Version_; }

    /// Returns `true` if the request has a body (non-zero Content-Length or chunked).
    bool HasBody() const;
    /**
     * @brief Reads and returns the entire request body as a string.
     *
     * Buffers all data up to `Content-Length` or until the final chunk.
     * Throws on connection close before body is complete.
     */
    TFuture<std::string> ReadBodyFull();
    /**
     * @brief Reads up to `size` bytes of the request body into `buffer`.
     *
     * Returns bytes read, `0` at end-of-body, or a negative retry hint.
     * Respects `Content-Length` and chunked encoding transparently.
     */
    TFuture<ssize_t> ReadBodySome(char* buffer, size_t size);
    /// Returns `true` once the full body has been read.
    bool BodyConsumed() const;
    /// Returns `true` if the client sent `Connection: close` or is HTTP/1.0.
    bool RequireConnectionClose() const;
    /// All request headers as a `name → value` map (views into the header buffer).
    const std::map<std::string_view, std::string_view>& Headers() const;

private:
    void ParseRequestLine();
    void ParseHeaders();

    TFuture<ssize_t> ReadBodySomeContentLength(char* buffer, size_t size);
    TFuture<ssize_t> ReadBodySomeChunked(char* buffer, size_t size);

    std::string Header_;
    size_t HeaderStartPos_ = 0;
    std::map<std::string_view, std::string_view> Headers_;
    size_t ContentLength_ = 0;
    bool HasBody_ = false;
    bool Chunked_ = false;
    bool BodyConsumed_ = false;
    size_t CurrentChunkSize_ = 0;
    std::function<TFuture<ssize_t>(char*, size_t)> BodyReader_;
    std::function<TFuture<std::string>()> ChunkHeaderReader_;
    std::string_view Method_;
    TUri Uri_;
    std::string_view Version_;
};

/**
 * @brief Builds and sends an HTTP/1.1 response.
 *
 * Typical usage in a router handler:
 * @code
 * response.SetStatus(200);
 * response.SetHeader("Content-Type", "text/plain");
 * co_await response.SendHeaders();
 * co_await response.WriteBodyFull("hello");    // sets Content-Length automatically
 * // -- or streaming --
 * co_await response.WriteBodyChunk(data, size); // Transfer-Encoding: chunked
 * @endcode
 *
 * `SetStatus` and `SetHeader` must be called **before** `SendHeaders`.
 * After `SendHeaders`, only body-write methods may be called.
 */
class TResponse {
public:
    TResponse(std::function<TFuture<ssize_t>(const void*, size_t)> writer)
        : Writer_(std::move(writer))
    {}
    /// Sets the HTTP status code (default is 200). Must be called before `SendHeaders`.
    void SetStatus(int statusCode);
    /// Adds or replaces a response header. Must be called before `SendHeaders`.
    void SetHeader(const std::string& name, const std::string& value);
    /**
     * @brief Sends the status line and all accumulated headers to the client.
     *
     * Must be called exactly once, after `SetStatus`/`SetHeader` and before any
     * body write. If `WriteBodyFull` is used instead, `SendHeaders` is called
     * implicitly.
     */
    TFuture<void> SendHeaders();
    /**
     * @brief Writes `data` as one chunk using Transfer-Encoding: chunked.
     *
     * Call `SendHeaders()` first (without a `Content-Length` header). Repeat for
     * each chunk; the final zero-length chunk is sent automatically when the
     * connection closes.
     */
    TFuture<void> WriteBodyChunk(const char* data, size_t size);
    /**
     * @brief Sends headers (with `Content-Length`) and the full body in one call.
     *
     * Implies `SendHeaders()` — do not call it separately when using this method.
     */
    TFuture<void> WriteBodyFull(const std::string& data);
    /// Returns `true` if the connection has been closed or an error occurred.
    bool IsClosed() const;
    /// Returns the status code set via `SetStatus` (default 200).
    int StatusCode() const {
        return StatusCode_;
    }

private:
    TFuture<void> CompleteWrite(const char* data, size_t size);

    int StatusCode_ = 200;
    std::map<std::string, std::string> Headers_;
    bool HeadersSent_ = false;
    bool Chunked_ = false;
    bool IsClosed_ = false;
    std::function<TFuture<ssize_t>(const void*, size_t)> Writer_;
};

/**
 * @brief Interface for HTTP request handlers.
 *
 * Implement `HandleRequest` to process requests and write responses. A single
 * router instance is shared across all connections; implementations must be
 * stateless or protect shared state explicitly (though a single-threaded event
 * loop means no concurrent calls in practice).
 *
 * @code
 * struct MyRouter : IRouter {
 *     TFuture<void> HandleRequest(TRequest& req, TResponse& res) override {
 *         res.SetStatus(200);
 *         res.SetHeader("Content-Type", "text/plain");
 *         co_await res.SendHeaders();
 *         co_await res.WriteBodyFull("hello");
 *     }
 * };
 * @endcode
 */
struct IRouter {
    virtual TFuture<void> HandleRequest(TRequest& request, TResponse& response) = 0;
};

class THelloWorldRouter : public IRouter {
public:
    TFuture<void> HandleRequest(TRequest& request, TResponse& response) override {
        if (request.Uri().Path() == "/") {
            response.SetStatus(200);
            response.SetHeader("Content-Type", "text/plain");
            response.SetHeader("Connection", "close");
            co_await response.SendHeaders();
            co_await response.WriteBodyFull("Hello, World!");
        } else {
            response.SetStatus(404);
            response.SetHeader("Content-Type", "text/plain");
            response.SetHeader("Connection", "close");
            co_await response.SendHeaders();
            co_await response.WriteBodyFull("Not Found");
        }
    }
};

/**
 * @brief HTTP/1.1 server that accepts connections and dispatches to a router.
 *
 * Takes ownership of a bound, listening socket. Call `Start()` to launch the
 * accept loop as a detached `TVoidTask`.
 *
 * @tparam TSocket A bound+listening socket type (e.g. `TDefaultPoller::TSocket`).
 *
 * @code
 * using TSocket = TDefaultPoller::TSocket;
 * TSocket sock(loop.Poller(), addr.Domain());
 * sock.Bind(addr); sock.Listen();
 *
 * MyRouter router;
 * TWebServer<TSocket> server(std::move(sock), router);
 * server.Start();
 * loop.Loop();
 * @endcode
 */
template<typename TSocket>
class TWebServer {
public:
    /**
     * @brief Constructs the server.
     *
     * @param serverSocket Bound, listening socket (moved in; owned by the server).
     * @param router       Request handler; must outlive this server.
     * @param logger       Optional nginx-style access-log callback.
     */
    TWebServer(TSocket&& serverSocket, IRouter& router, std::function<void(const std::string&)> logger = {})
        : ServerSocket(std::move(serverSocket))
        , Router(router)
        , Logger(std::move(logger))
    {}

    /**
     * @brief Starts the accept loop as a detached `TVoidTask`.
     *
     * Runs forever, accepting connections and spawning a per-connection handler
     * coroutine for each. Returns immediately (the loop is detached).
     */
    TVoidTask Start() {
        while (true) {
            auto clientSocket = co_await ServerSocket.Accept();
            HandleClient(std::move(clientSocket));
        }
    }

private:
    TVoidTask HandleClient(TSocket clientSocket) {
        auto byteReader = TByteReader<TSocket>(clientSocket);

        auto bodyReader = [&](char* buffer, size_t size) -> TFuture<ssize_t> {
            co_return co_await byteReader.ReadSome(buffer, size);
        };

        auto chunkHeaderReader = [&]() -> TFuture<std::string> {
            co_return co_await byteReader.ReadUntil("\r\n");
        };

        auto bodyWriter = [&](const void* data, size_t size) -> TFuture<ssize_t> {
            co_return co_await clientSocket.WriteSome(data, size);
        };

        std::string clientString = clientSocket.RemoteAddr() ? clientSocket.RemoteAddr()->ToString() : "unknown";

        try {
            while (true) {
                auto header = co_await byteReader.ReadUntil("\r\n\r\n");
                TRequest request(std::move(header), bodyReader, chunkHeaderReader);
                TResponse response(bodyWriter);
                co_await Router.HandleRequest(request, response);
                Log(request, response, clientString);
                if (response.IsClosed() || request.RequireConnectionClose()) {
                    break;
                }
            }
        } catch (const std::exception& ex) {
            if (Logger) {
                Logger(std::string("Client handler exception: ") + ex.what());
            }
        }
        co_return;
    }

    void Log(const TRequest& request, const TResponse& response, const std::string& clientString) {
        if (!Logger) {
            return;
        }
        LogStream.str("");
        LogStream.clear();

        // Build full path with query parameters if any
        std::string fullPath = request.Uri().Path();
        const auto& qp = request.Uri().QueryParameters();
        if (!qp.empty()) {
            fullPath.push_back('?');
            bool first = true;
            for (const auto& [k,v] : qp) {
                if (!first) fullPath.push_back('&');
                first = false;
                fullPath.append(k);
                fullPath.push_back('=');
                fullPath.append(v);
            }
        }

        // Timestamp in nginx style: [08/Nov/2025:15:23:10 +0100]
        auto now = std::chrono::system_clock::now();
        std::time_t raw = std::chrono::system_clock::to_time_t(now);
        std::tm localTm{};
        std::tm gmTm{};
#if defined(_WIN32)
        localtime_s(&localTm, &raw);
        gmtime_s(&gmTm, &raw);
#else
        localtime_r(&raw, &localTm);
        gmtime_r(&raw, &gmTm);
#endif
        // Compute timezone offset
        // mktime converts tm in local time to time_t; difference gives offset vs UTC
        std::time_t localTime = std::mktime(&localTm);
        std::time_t gmTime = std::mktime(&gmTm);
        long tzOffsetSec = static_cast<long>(difftime(localTime, gmTime));
        int tzSign = tzOffsetSec >= 0 ? 1 : -1;
        tzOffsetSec = std::labs(tzOffsetSec);
        int tzHours = static_cast<int>(tzOffsetSec / 3600);
        int tzMins = static_cast<int>((tzOffsetSec % 3600) / 60);
        char tzBuf[8];
        std::snprintf(tzBuf, sizeof(tzBuf), "%c%02d%02d", tzSign > 0 ? '+' : '-', tzHours, tzMins);

        // Prepare HTTP version string
        std::string_view ver = request.Version();
        bool hasHttpPrefix = ver.size() >= 5 && ver.substr(0,5) == "HTTP/";

        LogStream << clientString << " - - [" << std::put_time(&localTm, "%d/%b/%Y:%H:%M:%S ") << tzBuf << "] \""
                  << request.Method() << ' ' << fullPath << ' ' << (hasHttpPrefix ? std::string(ver) : (std::string("HTTP/") + std::string(ver))) << "\" "
                  << response.StatusCode() << ' ' << '-' << ' ' << "\"-\" \"-\"";
        Logger(LogStream.str());
    }

    TSocket ServerSocket;
    IRouter& Router;
    std::function<void(const std::string&)> Logger;
    std::ostringstream LogStream;
};

} // namespace NNet {