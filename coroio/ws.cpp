#include "ws.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <sstream>
#include <stdexcept>

namespace NNet
{

namespace {

std::string Base64Encode(const unsigned char* data, size_t dataLen) {
    std::string out;
    out.resize(4 * ((dataLen + 2) / 3));

    int outLen = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(&out[0]),
        data,
        static_cast<int>(dataLen)
    );

    if (outLen < 0) {
        return {};
    }
    out.resize(outLen);
    return out;
}

std::string FindSecWebSocketAccept(const std::string& response) {
    std::string lower = response;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    const std::string headerName = "sec-websocket-accept:";
    auto pos = lower.find(headerName);
    if (pos == std::string::npos) {
        return {};
    }
    pos += headerName.size();

    while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos]))) {
        pos++;
    }

    size_t startValue = pos;
    while (pos < lower.size() && lower[pos] != '\r' && lower[pos] != '\n') {
        pos++;
    }
    size_t endValue = pos;

    std::string val = response.substr(startValue, endValue - startValue);

    while (!val.empty() && std::isspace((unsigned char)val.back())) {
        val.pop_back();
    }

    return val;
}

std::string CalculateSecWebSocketAccept(const std::string& clientKeyBase64) {
    static const std::string magicGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string toSha = clientKeyBase64 + magicGUID;

    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(toSha.data()), toSha.size(), sha);

    return Base64Encode(sha, SHA_DIGEST_LENGTH);
}

} // namespace

std::string GenerateWebSocketKey(std::random_device& rd) {
    std::vector<uint8_t> randomBytes(16);
    std::generate(randomBytes.begin(), randomBytes.end(), std::ref(rd));
    return Base64Encode(randomBytes.data(), randomBytes.size());
}

void CheckSecWebSocketAccept(const std::string& allServerHeaders, const std::string& clientKeyBase64)
{
    std::string acceptFromServer = FindSecWebSocketAccept(allServerHeaders);
    if (acceptFromServer.empty()) {
        throw std::invalid_argument("No 'Sec-WebSocket-Accept' header found!");
    }

    std::string expected = CalculateSecWebSocketAccept(clientKeyBase64);

    if (acceptFromServer != expected) {
        std::ostringstream oss;
        oss << "Sec-WebSocket-Accept mismatch!\n"
            << " Server:   [" << acceptFromServer << "]\n"
            << " Expected: [" << expected << "]\n";
        throw std::invalid_argument(oss.str());
    }
}

} // namespace NNet
