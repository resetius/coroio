#pragma once

#include <coroio/sockutils.hpp>

#if defined(__linux__)
#include <arpa/inet.h>
#include <endian.h>
#define htonll(x) htobe64(x)
#define ntohll(x) be64toh(x)
#elif defined(__APPLE__)
#include <arpa/inet.h>
#elif defined(__FreeBSD__)
#include <arpa/inet.h>
#include <sys/endian.h>
#define htonll(x) htobe64(x)
#define ntohll(x) be64toh(x)
#elif defined(_WIN32)
#include <WinSock2.h>
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0A00)
inline uint64_t htonll(uint64_t value) {
#if BYTE_ORDER == LITTLE_ENDIAN || defined(_M_IX86) || defined(_M_X64)
    return ((uint64_t)htonl(static_cast<uint32_t>(value & 0xFFFFFFFF)) << 32) |
            htonl(static_cast<uint32_t>(value >> 32));
#else
    return value;
#endif
}

inline uint64_t ntohll(uint64_t value) {
#if BYTE_ORDER == LITTLE_ENDIAN || defined(_M_IX86) || defined(_M_X64)
    return ((uint64_t)ntohl(static_cast<uint32_t>(value & 0xFFFFFFFF)) << 32) |
            ntohl(static_cast<uint32_t>(value >> 32));
#else
    return value;
#endif
}
#endif // _WIN32_WINNT < 0x0A00
#endif

#include <random>

namespace NNet
{

namespace NDetail {

std::string GenerateWebSocketKey(std::random_device& rd);
void CheckSecWebSocketAccept(const std::string& allServerHeaders, const std::string& clientKeyBase64);

} // namespace detail

/**
 * @brief Client-side WebSocket framing layer over an already-connected socket.
 *
 * Wraps `TSocket` with RFC 6455 framing. The underlying socket must be TCP-connected
 * before calling `Connect()`. `TWebSocket` holds a **reference** to the socket —
 * the socket must outlive the `TWebSocket` instance.
 *
 * Only text frames are exposed. Outgoing frames are client-masked as required by
 * RFC 6455. Server-side upgrade is not included.
 *
 * @tparam TSocket Connected socket type providing `ReadSome` / `WriteSome`.
 *
 * @code
 * template<typename TPoller>
 * TFuture<void> chat(TPoller& poller, TAddress addr) {
 *     typename TPoller::TSocket sock(poller, addr.Domain());
 *     co_await sock.Connect(addr);
 *
 *     TWebSocket ws(sock);
 *     co_await ws.Connect("example.com", "/chat");   // HTTP upgrade
 *
 *     co_await ws.SendText("hello");
 *     std::string_view msg = co_await ws.ReceiveText(); // valid until next ReceiveText()
 * }
 * @endcode
 */
template<typename TSocket>
class TWebSocket {
public:
    /**
     * @brief Wraps an already-connected socket with WebSocket framing.
     *
     * @param socket Reference to the connected socket. Must outlive this object.
     */
    explicit TWebSocket(TSocket& socket)
        : Socket(socket)
        , Reader(socket)
        , Writer(socket)
    { }

    /**
     * @brief Performs the HTTP → WebSocket upgrade handshake.
     *
     * Sends an HTTP/1.1 GET request with `Upgrade: websocket` headers, then reads
     * and validates the server's `101 Switching Protocols` response including the
     * `Sec-WebSocket-Accept` header. Must be called once before `SendText` /
     * `ReceiveText`.
     *
     * @param host Sent in the `Host:` header (e.g. `"example.com"`).
     * @param path Request path (e.g. `"/chat"`).
     * @throws std::runtime_error if the server rejects the upgrade or the accept
     *         key does not match.
     */
    TFuture<void> Connect(const std::string& host, const std::string& path) {
        auto key = NDetail::GenerateWebSocketKey(Rd);
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

        NDetail::CheckSecWebSocketAccept(response, key);

        if (response.find("101 Switching Protocols") == std::string::npos) {
            throw std::runtime_error("Failed to establish WebSocket connection");
        }

        co_return;
    }

    /**
     * @brief Sends `message` as a masked WebSocket text frame (opcode 0x1).
     *
     * @param message UTF-8 text to send. The caller retains ownership.
     */
    TFuture<void> SendText(std::string_view message) {
        co_await SendFrame(0x1, message);
    }

    /**
     * @brief Receives the next WebSocket text frame.
     *
     * Suspends until a complete frame arrives. The returned `string_view` points
     * into an internal buffer and is **valid only until the next call to
     * `ReceiveText()`**. Copy the contents if you need to keep them longer.
     *
     * @return `string_view` over the frame payload.
     * @throws std::runtime_error if a non-text frame (opcode ≠ 0x1) is received.
     */
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

    TFuture<void> SendFrame(uint8_t opcode, std::string_view payload) {
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

    TFuture<std::pair<uint8_t, std::string_view>> ReceiveFrame() {
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
