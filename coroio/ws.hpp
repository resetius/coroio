#pragma once

#include "sockutils.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <endian.h>
#define htonll(x) htobe64(x)
#define ntohll(x) be64toh(x)
#elif defined(__APPLE__)
#include <arpa/inet.h>
#elif defined(_WIN32)
#include <WinSock2.h>
#endif

#include <random>

namespace NNet
{

std::string GenerateWebSocketKey(std::random_device& rd);
void CheckSecWebSocketAccept(const std::string& allServerHeaders, const std::string& clientKeyBase64);

template<typename TSocket>
class TWebSocket {
public:
    explicit TWebSocket(TSocket& socket)
        : Socket(socket)
        , Reader(socket)
        , Writer(socket)
    { }

    TFuture<void> Connect(const std::string& host, const std::string& path) {
        auto key = GenerateWebSocketKey(Rd);
        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "User-Agent: coroio\r\n"
            "Accept: */*\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";

        co_await Writer.Write(request.data(), request.size());

        auto response = co_await Reader.ReadUntil("\r\n\r\n");

        CheckSecWebSocketAccept(response, key);

        if (response.find("101 Switching Protocols") == std::string::npos) {
            throw std::runtime_error("Failed to establish WebSocket connection");
        }

        co_return;
    }

    TFuture<void> SendText(std::string_view message) {
        co_await SendFrame(0x1, message);
    }

    TFuture<std::string_view> ReceiveText() {
        auto [opcode, payload] = co_await ReceiveFrame();
        if (opcode != 0x1) {
            throw std::runtime_error(
                "Unexpected opcode: " +
                std::to_string(opcode) +
                " , expected text frame, got: '" +
                std::string(payload) + "'");
        }
        co_return payload;
    }

private:
    TSocket& Socket;
    TByteReader<TSocket> Reader;
    TByteWriter<TSocket> Writer;
    std::random_device Rd;
    std::string Payload;
    std::vector<uint8_t> Frame;

    TValueTask<void> SendFrame(uint8_t opcode, std::string_view payload) {
        Frame.clear();
        Frame.push_back(0x80 | opcode);

        uint8_t maskingKey[4];
        for (int i = 0; i < 4; ++i) {
            maskingKey[i] = static_cast<uint8_t>(Rd());
        }

        if (payload.size() <= 125) {
            Frame.push_back(0x80 | static_cast<uint8_t>(payload.size()));
        } else if (payload.size() <= 0xFFFF) {
            Frame.push_back(0x80 | 126);
            uint16_t length = htons(static_cast<uint16_t>(payload.size()));
            Frame.insert(Frame.end(), reinterpret_cast<uint8_t*>(&length), reinterpret_cast<uint8_t*>(&length) + 2);
        } else {
            Frame.push_back(0x80 | 127);
            uint64_t length = htonll(payload.size());
            Frame.insert(Frame.end(), reinterpret_cast<uint8_t*>(&length), reinterpret_cast<uint8_t*>(&length) + 8);
        }

        Frame.insert(Frame.end(), std::begin(maskingKey), std::end(maskingKey));

        for (size_t i = 0; i < payload.size(); ++i) {
            Frame.push_back(payload[i] ^ maskingKey[i % 4]);
        }

        co_await Writer.Write(Frame.data(), Frame.size());
        co_return;
    }

    TValueTask<std::pair<uint8_t, std::string_view>> ReceiveFrame() {
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

        Payload.resize(payloadLength);
        co_await Reader.Read(Payload.data(), Payload.size());

        if (masked) {
            for (size_t i = 0; i < Payload.size(); ++i) {
                Payload[i] ^= mask[i % 4];
            }
        }

        co_return {opcode, Payload};
    }
};

} // namespace NNet {