#include <chrono>
#include <deque>
#include <array>
#include <exception>
#include <sstream>
#include <stddef.h>
#include <signal.h>
#include <iostream>
#include <unordered_set>

#if defined(__SANITIZE_ADDRESS__) && defined(__linux__)
#include <sanitizer/lsan_interface.h>
#endif

#include <coroio/all.hpp>
#include <coroio/http/httpd.hpp>

#include "testlib.h"
#include "perf.h"

using namespace NNet;

void test_http_uri_parsing() {
    {
        NNet::TUri uri("/path/to/resource?param1=value1&param2=value2#fragment");
        ASSERT_STREQ(uri.Path().c_str(), "/path/to/resource");
        auto params = uri.QueryParameters();
        ASSERT_TRUE(params.size() == 2);
        ASSERT_STREQ(params["param1"].c_str(), "value1");
        ASSERT_STREQ(params["param2"].c_str(), "value2");
        ASSERT_STREQ(uri.Fragment().c_str(), "fragment");
    }
    {
        NNet::TUri uri("/simple/path");
        ASSERT_STREQ(uri.Path().c_str(), "/simple/path");
        auto params = uri.QueryParameters();
        ASSERT_TRUE(params.empty());
        ASSERT_STREQ(uri.Fragment().c_str(), "");
    }
    {
        NNet::TUri uri("/path/with/fragment#onlyfragment");
        ASSERT_STREQ(uri.Path().c_str(), "/path/with/fragment");
        auto params = uri.QueryParameters();
        ASSERT_TRUE(params.empty());
        ASSERT_STREQ(uri.Fragment().c_str(), "onlyfragment");
    }
    // encoded variants
    {
        NNet::TUri uri("/path%20with%20spaces?param%201=value%201#frag%20ment");
        ASSERT_STREQ(uri.Path().c_str(), "/path with spaces");
        auto params = uri.QueryParameters();
        ASSERT_TRUE(params.size() == 1);
        ASSERT_STREQ(params["param 1"].c_str(), "value 1");
        ASSERT_STREQ(uri.Fragment().c_str(), "frag ment");
    }
}

void test_http_request_handling_basic() {
    // Basic request parsing test
    std::string rawRequest =
        "GET /test/path?arg=value#frag HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Hello World";

    std::vector<char> requestData(rawRequest.begin(), rawRequest.end());
    size_t headerEndPos = rawRequest.find("\r\n\r\n") + 4;
    std::string headerPart(rawRequest.begin(), rawRequest.begin() + headerEndPos);

    auto bodyReader = [requestData, headerEndPos](char* buffer, size_t size) -> TFuture<ssize_t> {
        size_t bodyStart = headerEndPos;
        size_t bodySize = requestData.size() - bodyStart;
        size_t toRead = std::min(size, bodySize);
        std::memcpy(buffer, requestData.data() + bodyStart, toRead);
        co_return toRead;
    };

    TRequest request(std::move(headerPart), bodyReader);

    ASSERT_TRUE(request.Method() == "GET");
    ASSERT_STREQ(request.Uri().Path().c_str(), "/test/path");
    auto params = request.Uri().QueryParameters();
    ASSERT_TRUE(params.size() == 1);
    ASSERT_STREQ(params["arg"].c_str(), "value");

    std::string body;
    [](TRequest& request, std::string& body) -> TVoidTask {
        body = co_await request.ReadBodyFull();
    } (request, body);
    ASSERT_STREQ(body.c_str(), "Hello World");
}

void test_http_request_handling_advanced() {
    // part of body in raw data, rest via body reader
    std::string rawRequestPart =
        "POST /submit/data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 23\r\n"
        "\r\n"
        "PartialBody"
        "MoreBodyData";

    std::vector<char> requestData(rawRequestPart.begin(), rawRequestPart.end());
    size_t headerEndPos = rawRequestPart.find("\r\n\r\n") + 4;
    std::string headerPart(rawRequestPart.begin(), rawRequestPart.begin() + headerEndPos);

    auto bodyReader = [rawRequestPart, headerEndPos](char* buffer, size_t size) -> TFuture<ssize_t> {
        size_t bodyStart = headerEndPos;
        size_t bodySize = rawRequestPart.size() - bodyStart;
        size_t toRead = std::min(size, bodySize);
        std::memcpy(buffer, rawRequestPart.data() + bodyStart, toRead);
        co_return toRead;
    };

    TRequest request(std::move(headerPart), bodyReader);

    ASSERT_TRUE(request.Method() == "POST");
    ASSERT_STREQ(request.Uri().Path().c_str(), "/submit/data");
    auto params = request.Uri().QueryParameters();
    ASSERT_TRUE(params.empty());

    std::string body;
    [](TRequest& request, std::string& body) -> TVoidTask {
        try {
            body = co_await request.ReadBodyFull();
        } catch (const std::exception& e) {
            std::cerr << "Error reading body: " << e.what() << "\n";
        }
    } (request, body);
    ASSERT_STREQ(body.c_str(), "PartialBodyMoreBodyData");
}

void test_http_request_handling_2requests_in_1buffer() {
    // Two requests in one buffer
    std::string rawRequests =
        "GET /first HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello"
        "POST /second HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello";

    std::vector<char> requestData(rawRequests.begin(), rawRequests.end());
    // Check parse of first request
    size_t firstHeaderEndPos = rawRequests.find("\r\n\r\n") + 4;
    std::string firstHeaderPart(rawRequests.begin(), rawRequests.begin() + firstHeaderEndPos);

    auto firstBodyReader = [requestData, firstHeaderEndPos](char* buffer, size_t size) -> TFuture<ssize_t> {
        size_t bodyStart = firstHeaderEndPos;
        size_t bodySize = 5; // known from Content-Length
        size_t toRead = std::min(size, bodySize);
        std::memcpy(buffer, requestData.data() + bodyStart, toRead);
        co_return toRead;
    };
    TRequest firstRequest(std::move(firstHeaderPart), firstBodyReader);
    ASSERT_TRUE(firstRequest.Method() == "GET");
    ASSERT_STREQ(firstRequest.Uri().Path().c_str(), "/first");
    std::string firstBody;
    [](TRequest& request, std::string& body) -> TVoidTask {
        body = co_await request.ReadBodyFull();
    } (firstRequest, firstBody);
    ASSERT_STREQ(firstBody.c_str(), "Hello");
}

void test_http_request_handling_chunked_body() {
    // Chunked transfer encoding request
    std::string rawRequest =
        "POST /chunked HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";

    std::vector<char> requestData(rawRequest.begin(), rawRequest.end());
    size_t headerEndPos = rawRequest.find("\r\n\r\n") + 4;
    std::string headerPart(rawRequest.begin(), rawRequest.begin() + headerEndPos);
    size_t bodyStartPos = headerEndPos;
    std::string bodyPart(rawRequest.begin() + bodyStartPos, rawRequest.end());

    auto bodyPos = 0;

    auto bodyReader = [&bodyPart, &bodyPos](char* buffer, size_t size) -> TFuture<ssize_t> {
        size_t bodySize = bodyPart.size() - bodyPos;
        size_t toRead = std::min(size, bodySize);
        std::memcpy(buffer, bodyPart.data() + bodyPos, toRead);
        bodyPos += toRead;
        co_return toRead;
    };

    auto chunkHeaderReader = [&bodyPart, &bodyPos]() -> TFuture<std::string> {
        // read until \r\n from bodyPart
        std::string result;
        auto oldPos = bodyPos;
        bodyPos = bodyPart.find("\r\n", oldPos);
        if (bodyPos != std::string::npos) {
            result = bodyPart.substr(oldPos, bodyPos - oldPos + 2);
        }
        bodyPos += 2;
        co_return result;
    };

    TRequest request(std::move(headerPart), bodyReader, chunkHeaderReader);
    ASSERT_TRUE(request.Method() == "POST");
    ASSERT_STREQ(request.Uri().Path().c_str(), "/chunked");

    std::string body;
    [](TRequest& request, std::string& body) -> TVoidTask {
        try {
            body = co_await request.ReadBodyFull();
        } catch (const std::exception& e) {
            std::cerr << "Error reading request body: " << e.what() << std::endl;
        }
    } (request, body);
    ASSERT_STREQ(body.c_str(), "Hello World");
}

void test_http_response_handling_basic() {
    std::string responseData;
    auto writer = [&](const void* data, size_t size) -> TFuture<ssize_t> {
        std::string str(static_cast<const char*>(data), size);
        responseData += str;
        co_return size;
    };

    TResponse response(writer);
    response.SetStatus(200);
    response.SetHeader("Content-Type", "text/plain");
    response.SetHeader("Connection", "close");

    [](TResponse& response) -> TVoidTask {
        co_await response.SendHeaders();
        co_await response.WriteBodyFull("Hello, World!");
    } (response);

    std::string expectedResponse =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello, World!";
    ASSERT_STREQ(responseData.c_str(), expectedResponse.c_str());
}

void test_http_response_handling_chunked() {
    std::string responseData;
    auto writer = [&](const void* data, size_t size) -> TFuture<ssize_t> {
        std::string str(static_cast<const char*>(data), size);
        responseData += str;
        co_return size;
    };

    TResponse response(writer);
    response.SetStatus(200);
    response.SetHeader("Content-Type", "text/plain");
    response.SetHeader("Transfer-Encoding", "chunked");

    [](TResponse& response) -> TVoidTask {
        co_await response.SendHeaders();
        co_await response.WriteBodyChunk("Hello, ", 7);
        co_await response.WriteBodyChunk("World!", 6);
        co_await response.WriteBodyChunk("", 0); // final chunk
    } (response);

    std::string expectedResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "Hello, \r\n"
        "6\r\n"
        "World!\r\n"
        "0\r\n"
        "\r\n";
    ASSERT_STREQ(responseData.c_str(), expectedResponse.c_str());
}

void test_http_headers_case_insensitive() {
    std::string headerPart =
        "POST /x HTTP/1.1\r\n"
        "host: localhost\r\n"
        "CONTENT-LENGTH: 5\r\n"
        "transfer-Encoding: identity\r\n"
        "cOnNeCtIoN: close\r\n"
        "\r\n";
    auto bodyReader = [](char* buffer, size_t size) -> TFuture<ssize_t> {
        size_t n = std::min<size_t>(size, 5);
        std::memcpy(buffer, "Hello", n);
        co_return n;
    };
    TRequest request(std::move(headerPart), bodyReader);

    const auto& headers = request.Headers();
    ASSERT_TRUE(headers.find("Host") != headers.end());
    ASSERT_TRUE(headers.find("HOST") != headers.end());
    ASSERT_TRUE(headers.find("content-length")->second == "5");
    ASSERT_TRUE(headers.find("Transfer-Encoding")->second == "identity");
    ASSERT_TRUE(headers.find("Connection")->second == "close");
    ASSERT_TRUE(request.RequireConnectionClose());
    ASSERT_TRUE(headers.size() == 4);

    std::string body;
    [](TRequest& request, std::string& body) -> TVoidTask {
        body = co_await request.ReadBodyFull();
    } (request, body);
    ASSERT_STREQ(body.c_str(), "Hello");
}

void test_http_response_headers_case_insensitive() {
    std::string responseData;
    auto writer = [&](const void* data, size_t size) -> TFuture<ssize_t> {
        responseData.append(static_cast<const char*>(data), size);
        co_return size;
    };

    TResponse response(writer);
    response.SetHeader("Content-Type", "text/plain");
    response.SetHeader("content-type", "text/html");
    response.SetHeader("CONNECTION", "close");

    [](TResponse& response) -> TVoidTask {
        co_await response.WriteBodyFull("Hi");
    } (response);

    // One entry per name, first spelling kept, last value wins;
    // Content-Length is not added twice and Connection: close is recognized.
    std::string expectedResponse =
        "HTTP/1.1 200 OK\r\n"
        "CONNECTION: close\r\n"
        "Content-Length: 2\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "Hi";
    ASSERT_STREQ(responseData.c_str(), expectedResponse.c_str());
    ASSERT_TRUE(response.IsClosed());
}

namespace {

// Delivers the given chunks one per ReadSome; reports a closed connection
// when they run out, so a missed delimiter fails the test instead of hanging it.
struct TChunkedSocket {
    std::deque<std::string> Chunks;

    TFuture<ssize_t> ReadSome(void* data, size_t size) {
        if (Chunks.empty()) {
            co_return 0;
        }
        auto& chunk = Chunks.front();
        size_t n = std::min(size, chunk.size());
        std::memcpy(data, chunk.data(), n);
        chunk.erase(0, n);
        if (chunk.empty()) {
            Chunks.pop_front();
        }
        co_return n;
    }
};

// Reads up to the delimiter from the given chunks and then the rest of the data.
void CheckReadUntil(std::deque<std::string> chunks, const std::string& delimiter,
                    const std::string& expected, const std::string& rest)
{
    TChunkedSocket socket{std::move(chunks)};
    std::string got, gotRest;
    bool failed = false;
    auto task = [](TChunkedSocket& socket, const std::string& delimiter, std::string& got,
                   std::string& gotRest, size_t restSize, bool& failed) -> TFuture<void>
    {
        TByteReader<TChunkedSocket> reader(socket);
        try {
            got = co_await reader.ReadUntil(delimiter);
            gotRest.resize(restSize);
            co_await reader.Read(gotRest.data(), restSize);
        } catch (const std::exception&) {
            failed = true;
        }
    }(socket, delimiter, got, gotRest, rest.size(), failed);
    ASSERT_TRUE(task.done());
    ASSERT_FALSE(failed);
    ASSERT_TRUE(got == expected);
    ASSERT_TRUE(gotRest == rest);
}

} // namespace

void test_byte_reader_read_until_split_two_chunks() {
    const std::string delimiter = "\r\n\r\n";
    const std::string head = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string rest = "BODY";
    const std::string data = head + rest;
    for (size_t i = 1; i < data.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(CheckReadUntil({data.substr(0, i), data.substr(i)}, delimiter, head, rest));
    }
}

void test_byte_reader_read_until_byte_by_byte() {
    const std::string head = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string rest = "BODY";
    std::deque<std::string> chunks;
    for (char c : head + rest) {
        chunks.emplace_back(1, c);
    }
    ASSERT_NO_FATAL_FAILURE(CheckReadUntil(std::move(chunks), "\r\n\r\n", head, rest));
}

void test_byte_reader_read_until_1024_boundary() {
    const std::string delimiter = "\r\n\r\n";
    const std::string rest = "BODY";
    // The delimiter starts at offset 1010..1030, covering every split around the
    // 1024-byte read of ReadUntil
    for (size_t size = 1010; size <= 1030; ++size) {
        std::string head = std::string(size, 'x') + delimiter;
        ASSERT_NO_FATAL_FAILURE(CheckReadUntil({head + rest}, delimiter, head, rest));
    }
}

void test_byte_reader_read_until_two_delimiters() {
    TChunkedSocket socket{{"line1\r", "\nline2\r\n", "tail"}};
    std::string line1, line2;
    auto task = [](TChunkedSocket& socket, std::string& line1, std::string& line2) -> TFuture<void> {
        TByteReader<TChunkedSocket> reader(socket);
        line1 = co_await reader.ReadUntil("\r\n");
        line2 = co_await reader.ReadUntil("\r\n");
    }(socket, line1, line2);
    ASSERT_TRUE(task.done());
    ASSERT_TRUE(line1 == "line1\r\n");
    ASSERT_TRUE(line2 == "line2\r\n");
}

void test_byte_reader_read_until_empty_delimiter() {
    TChunkedSocket socket{{"abc"}};
    std::string empty1, empty2, rest;
    bool failed = false;
    auto task = [](TChunkedSocket& socket, std::string& empty1, std::string& empty2,
                   std::string& rest, bool& failed) -> TFuture<void>
    {
        TByteReader<TChunkedSocket> reader(socket);
        try {
            // Empty buffer
            empty1 = co_await reader.ReadUntil("");
            char buf[3];
            co_await reader.Read(buf, 1);
            rest.assign(buf, 1);
            // Non-empty buffer: the read of 3 bytes left "bc" buffered
            empty2 = co_await reader.ReadUntil("");
            co_await reader.Read(buf, 2);
            rest.append(buf, 2);
        } catch (const std::exception&) {
            failed = true;
        }
    }(socket, empty1, empty2, rest, failed);
    ASSERT_TRUE(task.done());
    ASSERT_FALSE(failed);
    ASSERT_TRUE(empty1.empty());
    ASSERT_TRUE(empty2.empty());
    ASSERT_TRUE(rest == "abc");
}

void test_http_server_header_split_between_writes() {
    using TSocket = TDefaultPoller::TSocket;

    struct TRouter : IRouter {
        TFuture<void> HandleRequest(TRequest&, TResponse& response) override {
            response.SetStatus(200);
            co_await response.SendHeaders();
            co_await response.WriteBodyFull("ok");
        }
    } router;

    TLoop<TDefaultPoller> loop;
    auto& poller = loop.Poller();

    TAddress serverAddr{"127.0.0.1", GetFreePort()};
    TSocket listener(poller, serverAddr.Domain());
    listener.Bind(serverAddr);
    listener.Listen();

    TWebServer<TSocket> server(std::move(listener), router);
    {
        // The detached accept loop never finishes, its frame is intentionally left suspended
#if defined(__SANITIZE_ADDRESS__) && defined(__linux__)
        __lsan::ScopedDisabler lsanDisabler;
#endif
        server.Start();
    }

    TSocket client(poller, serverAddr.Domain());
    std::string response;
    auto task = [](TSocket& client, TAddress& addr, TPollerBase& poller, std::string& response) -> TFuture<void> {
        co_await client.Connect(addr);
        TByteWriter writer(client);
        const std::string first = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
        const std::string second = "\r\n";
        co_await writer.Write(first.data(), first.size());
        co_await poller.Sleep(std::chrono::milliseconds(50));
        co_await writer.Write(second.data(), second.size());
        char buf[1024];
        while (response.find("ok") == std::string::npos) {
            auto n = co_await client.ReadSome(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            response.append(buf, n);
        }
    }(client, serverAddr, poller, response);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.done() && std::chrono::steady_clock::now() < deadline) {
        loop.Step();
    }
    ASSERT_TRUE(task.done());
    ASSERT_TRUE(response.find("HTTP/1.1 200") == 0);
    ASSERT_TRUE(response.find("ok") != std::string::npos);
}

namespace {

struct TWriteBodyFullRouter : IRouter {
    bool SendHeadersFirst = false;

    TFuture<void> HandleRequest(TRequest&, TResponse& response) override {
        response.SetStatus(200);
        if (SendHeadersFirst) {
            response.SetHeader("Content-Length", "5");
            co_await response.SendHeaders();
        }
        co_await response.WriteBodyFull("hello");
    }
};

} // namespace

void test_http_write_body_full() {
    using TSocket = TDefaultPoller::TSocket;

    TWriteBodyFullRouter router;
    TLoop<TDefaultPoller> loop;
    auto& poller = loop.Poller();

    TAddress serverAddr{"127.0.0.1", GetFreePort()};
    TSocket listener(poller, serverAddr.Domain());
    listener.Bind(serverAddr);
    listener.Listen();

    TWebServer<TSocket> server(std::move(listener), router);
    {
        // The detached accept loop never finishes, its frame is intentionally left suspended
#if defined(__SANITIZE_ADDRESS__) && defined(__linux__)
        __lsan::ScopedDisabler lsanDisabler;
#endif
        server.Start();
    }

    auto fetch = [&](const std::string& path) {
        TSocket client(poller, serverAddr.Domain());
        std::string raw;
        auto task = [](TSocket& client, TAddress& addr, TPollerBase& poller, const std::string& path, std::string& raw) -> TFuture<void> {
            co_await client.Connect(addr);
            TByteWriter writer(client);
            const std::string first = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
            const std::string second = "\r\n";
            co_await writer.Write(first.data(), first.size());
            co_await poller.Sleep(std::chrono::milliseconds(50));
            co_await writer.Write(second.data(), second.size());
            char buf[1024];
            while (raw.find("hello") == std::string::npos) {
                auto n = co_await client.ReadSome(buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                raw.append(buf, n);
            }
        }(client, serverAddr, poller, path, raw);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.done() && std::chrono::steady_clock::now() < deadline) {
            loop.Step();
        }
        EXPECT_TRUE(task.done());
        return raw;
    };

    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    // WriteBodyFull sends the headers itself
    ASSERT_STREQ(fetch("/").c_str(), expected.c_str());
    // SendHeaders first, then WriteBodyFull: the same bytes, headers not duplicated
    router.SendHeadersFirst = true;
    ASSERT_STREQ(fetch("/").c_str(), expected.c_str());
}

// A handler that waits for its client to hang up, as one that stops work nobody
// waits for. Pollers without remote hang-up reports throw instead.
template<typename TPoller>
void test_http_server_wait_hang_up() {
    using TSocket = typename TPoller::TSocket;

    struct TRouter : IRouter {
        bool Started = false;
        bool HungUp = false;
        bool Unsupported = false;

        TFuture<void> HandleRequest(TRequest& request, TResponse&) override {
            Started = true;
            try {
                co_await request.WaitHangUp();
                HungUp = true;
            } catch (const std::system_error& ex) {
                Unsupported = ex.code() == std::errc::operation_not_supported;
            }
        }
    } router;

    TLoop<TPoller> loop;
    auto& poller = loop.Poller();

    TAddress serverAddr{"127.0.0.1", GetFreePort()};
    TSocket listener(poller, serverAddr.Domain());
    listener.Bind(serverAddr);
    listener.Listen();

    TWebServer<TSocket> server(std::move(listener), router);
    {
        // The detached accept loop never finishes, its frame is intentionally left suspended
#if defined(__SANITIZE_ADDRESS__) && defined(__linux__)
        __lsan::ScopedDisabler lsanDisabler;
#endif
        server.Start();
    }

    TSocket client(poller, serverAddr.Domain());
    auto task = [](TSocket& client, TAddress& addr) -> TFuture<void> {
        co_await client.Connect(addr);
        TByteWriter writer(client);
        // The connection's handler then ends with the router instead of waiting for another request.
        const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        co_await writer.Write(request.data(), request.size());
    }(client, serverAddr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(task.done() && router.Started) && std::chrono::steady_clock::now() < deadline) {
        loop.Step();
    }
    ASSERT_TRUE(task.done() && router.Started);
    task.await_resume();
    ASSERT_FALSE(router.HungUp);
    client.Close();
    while (!router.HungUp && !router.Unsupported && std::chrono::steady_clock::now() < deadline) {
        loop.Step();
    }
    ASSERT_EQ(router.HungUp, poller.SupportsRemoteHup());
    ASSERT_EQ(router.Unsupported, !poller.SupportsRemoteHup());
}

#define TEST_SUITE_NAME "test_http"

int main(int argc, char** argv) {
    TInitializer init;

    testing::InitGoogleTest(&argc, argv);

    ADD_TEST(unit_test, test_http_uri_parsing);
    ADD_TEST(unit_test, test_http_request_handling_basic);
    ADD_TEST(unit_test, test_http_request_handling_advanced);
    ADD_TEST(unit_test, test_http_request_handling_2requests_in_1buffer);
    ADD_TEST(unit_test, test_http_request_handling_chunked_body);
    ADD_TEST(unit_test, test_http_headers_case_insensitive);
    ADD_TEST(unit_test, test_http_response_headers_case_insensitive);
    ADD_TEST(unit_test, test_http_response_handling_basic);
    ADD_TEST(unit_test, test_http_response_handling_chunked);
    ADD_TEST(unit_test, test_byte_reader_read_until_split_two_chunks);
    ADD_TEST(unit_test, test_byte_reader_read_until_byte_by_byte);
    ADD_TEST(unit_test, test_byte_reader_read_until_1024_boundary);
    ADD_TEST(unit_test, test_byte_reader_read_until_two_delimiters);
    ADD_TEST(unit_test, test_byte_reader_read_until_empty_delimiter);
    ADD_TEST(unit_test, test_http_write_body_full);
    ADD_TEST(unit_test, test_http_server_header_split_between_writes);
    RegisterTest("test_http_server_wait_hang_up", "TSelect", test_http_server_wait_hang_up<TSelect>);
#if defined(__APPLE__) || defined(__FreeBSD__)
    RegisterTest("test_http_server_wait_hang_up", "TKqueue", test_http_server_wait_hang_up<TKqueue>);
#else
    RegisterTest("test_http_server_wait_hang_up", "TEPoll", test_http_server_wait_hang_up<TEPoll>);
#endif

    return RUN_ALL_TESTS();
}