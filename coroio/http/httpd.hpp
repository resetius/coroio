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

// /path?arg1=value1&arg2=value2#fragment
class TUri {
public:
    TUri() = default;
    TUri(const std::string& uriStr);
    const std::string& Path() const;
    const std::map<std::string, std::string>& QueryParameters() const;
    const std::string& Fragment() const;

private:
    void Parse(const std::string& uriStr);
    std::string Path_;
    std::map<std::string, std::string> QueryParameters_;
    std::string Fragment_;
};

class TRequest {
public:
    TRequest(std::string&& header,
        std::function<TFuture<ssize_t>(char*, size_t)> bodyReader,
        std::function<TFuture<std::string>()> chunkHeaderReader = {});
    std::string_view Method() const;
    const TUri& Uri() const;
    std::string_view Version() const { return Version_; }

    bool HasBody() const;
    TFuture<std::string> ReadBodyFull();
    TFuture<ssize_t> ReadBodySome(char* buffer, size_t size); // read up to Content-Length
    bool BodyConsumed() const;
    bool RequireConnectionClose() const;

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

class TResponse {
public:
    TResponse(std::function<TFuture<ssize_t>(const void*, size_t)> writer)
        : Writer_(std::move(writer))
    {}
    void SetStatus(int statusCode);
    void SetHeader(const std::string& name, const std::string& value);
    TFuture<void> SendHeaders();
    TFuture<void> WriteBodyChunk(const char* data, size_t size); // Chunked transfer encoding
    TFuture<void> WriteBodyFull(const std::string& data); // Content-Length + body
    bool IsClosed() const;
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

struct IRouter {
    virtual TFuture<void> HandleRequest(const TRequest& request, TResponse& response) = 0;
};

class THelloWorldRouter : public IRouter {
public:
    TFuture<void> HandleRequest(const TRequest& request, TResponse& response) override {
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

template<typename TSocket>
class TWebServer {
public:
    TWebServer(TSocket&& serverSocket, IRouter& router, std::function<void(const std::string&)> logger = {})
        : ServerSocket(std::move(serverSocket))
        , Router(router)
        , Logger(std::move(logger))
    {}

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