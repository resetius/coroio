#include "utils.hpp"

#include <cstdint>
#include <vector>
#include <cstring>
#include <sstream>

namespace NNet::NUtils {

namespace {

uint32_t rol(uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32 - bits));
}

} // namespace

std::string Base64Encode(const unsigned char* data, size_t dataLen) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.resize(4 * ((dataLen + 2) / 3));

    size_t even = (dataLen / 3) * 3;
    size_t rest = dataLen - even;
    size_t i = 0;

    for (; i < even; i += 3) {
        out[i / 3 * 4] = table[(data[i] >> 2) & 0x3F];
        out[i / 3 * 4 + 1] = table[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)];
        out[i / 3 * 4 + 2] = table[((data[i + 1] & 0x0F) << 2) | ((data[i + 2] >> 6) & 0x03)];
        out[i / 3 * 4 + 3] = table[data[i + 2] & 0x3F];
    }

    if (rest == 1) {
        out[i / 3 * 4] = table[(data[i] >> 2) & 0x3F];
        out[i / 3 * 4 + 1] = table[(data[i] & 0x03) << 4];
        out[i / 3 * 4 + 2] = '=';
        out[i / 3 * 4 + 3] = '=';
    } else if (rest == 2) {
        out[i / 3 * 4] = table[(data[i] >> 2) & 0x3F];
        out[i / 3 * 4 + 1] = table[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)];
        out[i / 3 * 4 + 2] = table[(data[i + 1] & 0x0F) << 2];
        out[i / 3 * 4 + 3] = '=';
    }

    out.resize(i / 3 * 4 + (rest > 0 ? 4 : 0));
    return out;
}

// https://en.wikipedia.org/wiki/SHA-1
void SHA1Digest(const unsigned char* data, size_t dataLen, unsigned char* output) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    size_t newLen = ((dataLen + 8) / 64 + 1) * 64;
    std::vector<unsigned char> buffer(newLen);
    memcpy(buffer.data(), data, dataLen);
    buffer[dataLen] = 0x80;

    size_t bitLen = dataLen * 8;
    for (int i = 0; i < 8; ++i) {
        buffer[newLen - 1 - i] = static_cast<unsigned char>((bitLen >> (8 * i)) & 0xFF);
    }

    for (size_t offset = 0; offset < newLen; offset += 64) {
        uint32_t w[80];
        for (int t = 0; t < 16; ++t) {
            size_t i = offset + t * 4;
            w[t] = (buffer[i] << 24) | (buffer[i+1] << 16) | (buffer[i+2] << 8) | buffer[i+3];
        }
        for (int t = 16; t < 80; ++t) {
            w[t] = rol(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        int t = 0;
        for (; t < 20; ++t) {
            uint32_t f = (b & c) | ((~b) & d);
            uint32_t k = 0x5A827999;
            uint32_t tmp = rol(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        for (; t < 40; ++t) {
            uint32_t f = b ^ c ^ d;
            uint32_t k = 0x6ED9EBA1;
            uint32_t tmp = rol(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        for (; t < 60; ++t) {
            uint32_t f = (b & c) | (b & d) | (c & d);
            uint32_t k = 0x8F1BBCDC;
            uint32_t tmp = rol(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        for (; t < 80; ++t) {
            uint32_t f = b ^ c ^ d;
            uint32_t k = 0xCA62C1D6;
            uint32_t tmp = rol(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    snprintf(reinterpret_cast<char*>(output), 41,
             "%08x%08x%08x%08x%08x",
             h0, h1, h2, h3, h4);
}

} // namespace NNet::NUtils
