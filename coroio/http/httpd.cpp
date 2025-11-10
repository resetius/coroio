#include "httpd.hpp"

namespace NNet
{

namespace {

std::string UrlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            char hex[3] = { str[i + 1], str[i + 2], 0 };
            char decodedChar = static_cast<char>(std::strtol(hex, nullptr, 16));
            result += decodedChar;
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }

    return result;
}

} // namespace

TUri::TUri(const std::string& uriStr) {
    Parse(uriStr);
}

void TUri::Parse(const std::string& uriStr) {
    size_t pathEnd = uriStr.find_first_of("?#");
    Path_ = UrlDecode(uriStr.substr(0, pathEnd));

    if (pathEnd != std::string::npos) {
        if (uriStr[pathEnd] == '?') {
            size_t queryEnd = uriStr.find('#', pathEnd);
            std::string queryStr = uriStr.substr(pathEnd + 1,
                queryEnd == std::string::npos ? std::string::npos : queryEnd - pathEnd - 1);

            size_t pos = 0;
            while (pos < queryStr.size()) {
                size_t ampPos = queryStr.find('&', pos);
                std::string param = queryStr.substr(pos, ampPos - pos);
                size_t eqPos = param.find('=');
                if (eqPos != std::string::npos) {
                    std::string name = UrlDecode(param.substr(0, eqPos));
                    std::string value = UrlDecode(param.substr(eqPos + 1));
                    QueryParameters_[name] = value;
                } else {
                    std::string name = UrlDecode(param);
                    QueryParameters_[name] = "";
                }
                if (ampPos == std::string::npos) {
                    break;
                }
                pos = ampPos + 1;
            }

            if (queryEnd != std::string::npos) {
                Fragment_ = UrlDecode(uriStr.substr(queryEnd + 1));
            }
        } else if (uriStr[pathEnd] == '#') {
            Fragment_ = UrlDecode(uriStr.substr(pathEnd + 1));
        }
    }
}

const std::string& TUri::Fragment() const {
    return Fragment_;
}

const std::map<std::string, std::string>& TUri::QueryParameters() const {
    return QueryParameters_;
}

const std::string& TUri::Path() const {
    return Path_;
}


TRequest::TRequest(std::string&& header,
    std::function<TFuture<ssize_t>(char*, size_t)> bodyReader,
    std::function<TFuture<std::string>()> chunkHeaderReader)
    : Header_(std::move(header))
    , BodyReader_(std::move(bodyReader))
    , ChunkHeaderReader_(std::move(chunkHeaderReader))
{
    // Parse the request line and headers
    ParseRequestLine();
    ParseHeaders();
}

void TRequest::ParseRequestLine() {
    size_t lineEnd = Header_.find("\r\n");
    if (lineEnd == std::string::npos) {
        throw std::runtime_error("Invalid HTTP request: no request line");
    }
    HeaderStartPos_ = lineEnd + 2;

    std::string_view requestLine(Header_.data(), lineEnd);
    size_t methodEnd = requestLine.find(' ');
    if (methodEnd == std::string::npos) {
        throw std::runtime_error("Invalid HTTP request: no method");
    }
    Method_ = requestLine.substr(0, methodEnd);

    size_t uriStart = methodEnd + 1;
    size_t uriEnd = requestLine.find(' ', uriStart);
    if (uriEnd == std::string::npos) {
        throw std::runtime_error("Invalid HTTP request: no URI");
    }
    Uri_ = TUri(std::string(requestLine.substr(uriStart, uriEnd - uriStart)));

    size_t versionStart = uriEnd + 1;
    if (versionStart >= requestLine.size()) {
        throw std::runtime_error("Invalid HTTP request: no version");
    }
    Version_ = requestLine.substr(versionStart);
}

void TRequest::ParseHeaders() {
    size_t pos = HeaderStartPos_;
    while (pos < Header_.size()) {
        size_t lineEnd = std::string_view(Header_.data() + pos,
            Header_.size() - pos).find("\r\n");
        if (lineEnd == std::string::npos) {
            break;
        }
        std::string_view headerLine(Header_.data() + pos, lineEnd);
        size_t colonPos = headerLine.find(':');
        if (colonPos != std::string::npos) {
            std::string_view name = headerLine.substr(0, colonPos);
            std::string_view value = headerLine.substr(colonPos + 1);
            // Trim leading spaces from value
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            Headers_[name] = value;
        }
        pos += lineEnd + 2;
    }

    auto maybeContentLength = Headers_.find("Content-Length");;
    if (maybeContentLength != Headers_.end()) {
        ContentLength_ = std::stoi(std::string(maybeContentLength->second));
        HasBody_ = ContentLength_ > 0;
    }

    auto maybeChunked = Headers_.find("Transfer-Encoding");;
    if (maybeChunked != Headers_.end()) {
        if (maybeChunked->second == "chunked") {
            Chunked_ = true;
        }
    }
}

const std::map<std::string_view, std::string_view>& TRequest::Headers() const {
    return Headers_;
}

bool TRequest::RequireConnectionClose() const {
    auto connectionHeader = Headers_.find("Connection");
    if (connectionHeader != Headers_.end()) {
        if (connectionHeader->second == "close") {
            return true;
        }
    }
    if (Version_ == "HTTP/1.0") {
        return true;
    }
    return false;
}

std::string_view TRequest::Method() const {
    return Method_;
}

const TUri& TRequest::Uri() const {
    return Uri_;
}

bool TRequest::HasBody() const {
    return HasBody_;
}

bool TRequest::BodyConsumed() const {
    return BodyConsumed_;
}

TFuture<std::string> TRequest::ReadBodyFull() {
    std::string body;
    if (!Chunked_) {
        body.reserve(ContentLength_);
        while (ContentLength_ > 0) {
            char buffer[4096];
            ssize_t bytesRead = co_await ReadBodySomeContentLength(buffer, std::min(sizeof(buffer), ContentLength_));
            if (bytesRead <= 0) {
                throw std::runtime_error("Error reading request body");
            }
            body.append(buffer, bytesRead);
        }
    } else {
        while (!BodyConsumed_) {
            char buffer[4096];
            ssize_t bytesRead = co_await ReadBodySomeChunked(buffer, sizeof(buffer));
            if (bytesRead < 0) {
                throw std::runtime_error("Error reading request body");
            }
            if (bytesRead == 0) {
                break;
            }
            body.append(buffer, bytesRead);
        }
    }
    co_return body;
}

TFuture<ssize_t> TRequest::ReadBodySome(char* buffer, size_t size) {
    if (BodyConsumed_) {
        co_return 0;
    }
    if (!Chunked_) {
        co_return co_await ReadBodySomeContentLength(buffer, size);
    } else {
        co_return co_await ReadBodySomeChunked(buffer, size);
    }
}

TFuture<ssize_t> TRequest::ReadBodySomeContentLength(char* buffer, size_t size) {
    // read up to Content-Length
    if (ContentLength_ == 0) {
        BodyConsumed_ = true;
        co_return 0;
    }
    ssize_t bytesRead = co_await BodyReader_(buffer, std::min(size, ContentLength_));
    if (bytesRead > 0) {
        ContentLength_ -= bytesRead;
    }
    if (ContentLength_ == 0) {
        BodyConsumed_ = true;
    }
    co_return bytesRead;
}

TFuture<ssize_t> TRequest::ReadBodySomeChunked(char* buffer, size_t size) {
    // Read size\r\n
    // Read data\r\n

    auto readCrLf = [&]() -> TFuture<void> {
        char crlf[2];
        auto size = co_await BodyReader_(crlf, 2);
        if (size != 2 || crlf[0] != '\r' || crlf[1] != '\n') {
            throw std::runtime_error("Invalid chunked encoding");
        }
        co_return;
    };

    if (CurrentChunkSize_ == 0) {
        auto line = co_await ChunkHeaderReader_();
        CurrentChunkSize_ = std::stoul(line, nullptr, 16);
        if (CurrentChunkSize_ == 0) {
            BodyConsumed_ = true;
            co_await readCrLf();
            co_return 0;
        }
    }

    size_t toRead = std::min(size, CurrentChunkSize_);
    ssize_t bytesRead = co_await BodyReader_(buffer, toRead);
    if (bytesRead > 0) {
        CurrentChunkSize_ -= bytesRead;
        if (CurrentChunkSize_ == 0) {
            co_await readCrLf();
        }
    }
    co_return bytesRead;
}

void TResponse::SetStatus(int statusCode) {
    StatusCode_ = statusCode;
}

void TResponse::SetHeader(const std::string& name, const std::string& value) {
    Headers_[name] = value;
}

TFuture<void> TResponse::CompleteWrite(const char* data, size_t size) {
    const char* p = data;
    size_t remaining = size;
    while (remaining != 0) {
        ssize_t written = co_await Writer_(p, remaining);
        if (written <= 0) {
            throw std::runtime_error("Error writing response body");
        }
        p += written;
        remaining -= written;
    }
    co_return;
}

TFuture<void> TResponse::SendHeaders() {
    // send response line and headers
    if (HeadersSent_) {
        co_return;
    }
    HeadersSent_ = true;

    std::string headerStr = "HTTP/1.1 " + std::to_string(StatusCode_) + " OK\r\n";
    for (const auto& header : Headers_) {
        headerStr += header.first + ": " + header.second + "\r\n";
    }
    headerStr += "\r\n";

    co_await CompleteWrite(headerStr.data(), headerStr.size());

    auto maybeChunked = Headers_.find("Transfer-Encoding");
    if (maybeChunked != Headers_.end()) {
        if (maybeChunked->second == "chunked") {
            Chunked_ = true;
        }
    }
    auto maybeIsClosed = Headers_.find("Connection");
    if (maybeIsClosed != Headers_.end()) {
        if (maybeIsClosed->second == "close") {
            IsClosed_ = true;
        }
    }
    co_return;
}

bool TResponse::IsClosed() const {
    return IsClosed_;
}

TFuture<void> TResponse::WriteBodyChunk(const char* data, size_t size) {
    // if chunked => send chunk size + \r\n + data + \r\n
    // else => send part of body
    if (Chunked_) {
        std::string chunkHeader = std::to_string(size) + "\r\n";
        co_await CompleteWrite(chunkHeader.data(), chunkHeader.size());
        co_await CompleteWrite(data, size);
        co_await CompleteWrite("\r\n", 2);
    } else {
        co_await CompleteWrite(data, size);
    }
}

TFuture<void> TResponse::WriteBodyFull(const std::string& data) {
    if (Chunked_) {
        // If chunked, we need to send the data in chunks
        size_t offset = 0;
        constexpr size_t chunkSize = 8192;
        while (offset < data.size()) {
            size_t toSend = std::min(data.size() - offset, chunkSize);
            co_await WriteBodyChunk(data.data() + offset, toSend);
            offset += toSend;
        }
    } else {
        // If not chunked, we can send the data all at once
        co_await CompleteWrite(data.data(), data.size());
    }
}

} // namespace NNet
