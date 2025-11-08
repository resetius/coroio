#include <chrono>
#include <array>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>
#include <iostream>
#include <unordered_set>

#include <coroio/all.hpp>
#include <coroio/http/httpd.hpp>

#include "testlib.h"
#include "perf.h"

extern "C" {
#include <cmocka.h>
}

using namespace NNet;

void test_http_uri_parsing(void**) {
    {
        NNet::TUri uri("/path/to/resource?param1=value1&param2=value2#fragment");
        assert_string_equal(uri.Path().c_str(), "/path/to/resource");
        auto params = uri.QueryParameters();
        assert_true(params.size() == 2);
        assert_string_equal(params["param1"].c_str(), "value1");
        assert_string_equal(params["param2"].c_str(), "value2");
        assert_string_equal(uri.Fragment().c_str(), "fragment");
    }
    {
        NNet::TUri uri("/simple/path");
        assert_string_equal(uri.Path().c_str(), "/simple/path");
        auto params = uri.QueryParameters();
        assert_true(params.empty());
        assert_string_equal(uri.Fragment().c_str(), "");
    }
    {
        NNet::TUri uri("/path/with/fragment#onlyfragment");
        assert_string_equal(uri.Path().c_str(), "/path/with/fragment");
        auto params = uri.QueryParameters();
        assert_true(params.empty());
        assert_string_equal(uri.Fragment().c_str(), "onlyfragment");
    }
    // encoded variants
    {
        NNet::TUri uri("/path%20with%20spaces?param%201=value%201#frag%20ment");
        assert_string_equal(uri.Path().c_str(), "/path with spaces");
        auto params = uri.QueryParameters();
        assert_true(params.size() == 1);
        assert_string_equal(params["param 1"].c_str(), "value 1");
        assert_string_equal(uri.Fragment().c_str(), "frag ment");
    }
}

void test_http_request_handling_basic(void**) {
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

    assert_true(request.Method() == "GET");
    assert_string_equal(request.Uri().Path().c_str(), "/test/path");
    auto params = request.Uri().QueryParameters();
    assert_true(params.size() == 1);
    assert_string_equal(params["arg"].c_str(), "value");

    std::string body;
    [](TRequest& request, std::string& body) -> TVoidTask {
        body = co_await request.ReadBodyFull();
    } (request, body);
    assert_string_equal(body.c_str(), "Hello World");
}

void test_http_request_handling_advanced(void**) {
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

    assert_true(request.Method() == "POST");
    assert_string_equal(request.Uri().Path().c_str(), "/submit/data");
    auto params = request.Uri().QueryParameters();
    assert_true(params.empty());

    std::string body;
    [](TRequest& request, std::string& body) -> TVoidTask {
        try {
            body = co_await request.ReadBodyFull();
        } catch (const std::exception& e) {
            std::cerr << "Error reading body: " << e.what() << "\n";
        }
    } (request, body);
    assert_string_equal(body.c_str(), "PartialBodyMoreBodyData");
}

void test_http_request_handling_2requests_in_1buffer(void**) {
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
    assert_true(firstRequest.Method() == "GET");
    assert_string_equal(firstRequest.Uri().Path().c_str(), "/first");
    std::string firstBody;
    [](TRequest& request, std::string& body) -> TVoidTask {
        body = co_await request.ReadBodyFull();
    } (firstRequest, firstBody);
    assert_string_equal(firstBody.c_str(), "Hello");
}

void test_http_request_handling_chunked_body(void**) {
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
    assert_true(request.Method() == "POST");
    assert_string_equal(request.Uri().Path().c_str(), "/chunked");

    std::string body;
    [](TRequest& request, std::string& body) -> TVoidTask {
        try {
            body = co_await request.ReadBodyFull();
        } catch (const std::exception& e) {
            std::cerr << "Error reading request body: " << e.what() << std::endl;
        }
    } (request, body);
    assert_string_equal(body.c_str(), "Hello World");
}

void test_http_response_handling_basic(void**) {
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
    assert_string_equal(responseData.c_str(), expectedResponse.c_str());
}

void test_http_response_handling_chunked(void**) {
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
    assert_string_equal(responseData.c_str(), expectedResponse.c_str());
}

int main(int argc, char** argv) {
    TInitializer init;

    std::vector<CMUnitTest> tests;
    std::unordered_set<std::string> filters;
    tests.reserve(500);

    parse_filters(argc, argv, filters);

    ADD_TEST(cmocka_unit_test, test_http_uri_parsing);
    ADD_TEST(cmocka_unit_test, test_http_request_handling_basic);
    ADD_TEST(cmocka_unit_test, test_http_request_handling_advanced);
    ADD_TEST(cmocka_unit_test, test_http_request_handling_2requests_in_1buffer);
    ADD_TEST(cmocka_unit_test, test_http_request_handling_chunked_body);
    ADD_TEST(cmocka_unit_test, test_http_response_handling_basic);
    ADD_TEST(cmocka_unit_test, test_http_response_handling_chunked);

    return _cmocka_run_group_tests("test_http", tests.data(), tests.size(), NULL, NULL);
}