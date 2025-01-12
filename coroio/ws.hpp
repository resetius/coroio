#pragma once

#include "sockutils.hpp"

#include <arpa/inet.h>
#include <openssl/evp.h>
#include <random>

namespace NNet
{

inline std::string GenerateWebSocketKey() {
    std::vector<uint8_t> randomBytes(16);
    std::random_device rd;
    std::generate(randomBytes.begin(), randomBytes.end(), std::ref(rd));

    EVP_ENCODE_CTX* ctx = EVP_ENCODE_CTX_new();
    std::string base64;
    base64.resize(4 * ((randomBytes.size() + 2) / 3));
    int outLen;
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&base64[0]), randomBytes.data(), randomBytes.size());

    return base64;
}

template<typename TSocket>
class TWebSocket {
public:
    explicit TWebSocket(TSocket& socket)
        : Socket(socket)
        , Reader(socket)
        , Writer(socket)
    { }

    TFuture<void> Connect(const std::string& host, const std::string& path) {
        auto key = GenerateWebSocketKey();
        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "User-Agent: curl/8.7.1\r\n"
            "Accept: */*\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";

        std::cerr << request << "\n";

        co_await Writer.Write(request.data(), request.size());

        auto response = co_await Reader.ReadUntil("\r\n\r\n");
        std::cerr << response << "\n";

        if (response.find("101 Switching Protocols") == std::string::npos) {
            throw std::runtime_error("Failed to establish WebSocket connection");
        }

        co_return;
    }

    TFuture<void> SendText(const std::string& message) {
        co_await SendFrame(0x1, message);
    }

    TFuture<std::string> ReceiveText() {
        auto [opcode, payload] = co_await ReceiveFrame();
        if (opcode != 0x1) {
            throw std::runtime_error(
                "Unexpected opcode: " +
                std::to_string(opcode) +
                " , expected text frame, got: '" +
                payload + "'");
        }
        co_return payload;
    }

private:
    TSocket& Socket;
    TByteReader<TSocket> Reader;
    TByteWriter<TSocket> Writer;


    TValueTask<void> SendFrame(uint8_t opcode, const std::string& payload) {
        std::vector<uint8_t> frame;

        frame.push_back(0x80 | opcode);

        uint8_t maskingKey[4];
        std::random_device rd; // TODO
        for (int i = 0; i < 4; ++i) {
            maskingKey[i] = static_cast<uint8_t>(rd());
        }

        if (payload.size() <= 125) {
            frame.push_back(0x80 | static_cast<uint8_t>(payload.size()));
        } else if (payload.size() <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            uint16_t length = htons(static_cast<uint16_t>(payload.size()));
            frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&length), reinterpret_cast<uint8_t*>(&length) + 2);
        } else {
            frame.push_back(0x80 | 127);
            uint64_t length = htonll(payload.size());
            frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&length), reinterpret_cast<uint8_t*>(&length) + 8);
        }

        frame.insert(frame.end(), std::begin(maskingKey), std::end(maskingKey));
        std::vector<uint8_t> maskedPayload(payload.begin(), payload.end());
        for (size_t i = 0; i < maskedPayload.size(); ++i) {
            maskedPayload[i] ^= maskingKey[i % 4];
        }
        frame.insert(frame.end(), maskedPayload.begin(), maskedPayload.end());

        co_await Writer.Write(frame.data(), frame.size());
        co_return;
    }

    TValueTask<std::pair<uint8_t, std::string>> ReceiveFrame() {
        uint8_t header[2];
        co_await Reader.Read(header, sizeof(header));

        uint8_t opcode = header[0] & 0x0F;
        bool masked = header[1] & 0x80;
        uint64_t payloadLength = header[1] & 0x7F;

        if (payloadLength == 126) {
            uint16_t extendedLength;
            co_await Reader.Read(&extendedLength, sizeof(extendedLength));
            payloadLength = ntohs(extendedLength);
        } else if (payloadLength == 127) {
            uint64_t extendedLength;
            co_await Reader.Read(&extendedLength, sizeof(extendedLength));
            payloadLength = ntohll(extendedLength);
        }

        uint8_t mask[4] = {0};
        if (masked) {
            co_await Reader.Read(mask, sizeof(mask));
        }

        std::vector<uint8_t> payload(payloadLength);
        co_await Reader.Read(payload.data(), payload.size());

        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= mask[i % 4];
            }
        }

        co_return {opcode, std::string(payload.begin(), payload.end())};
    }
};

} // namespace NNet {