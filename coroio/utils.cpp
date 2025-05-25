#include "utils.hpp"

namespace NNet::NUtils {

std::string Base64Encode([[maybe_unused]] const unsigned char* data, [[maybe_unused]] size_t dataLen) {
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

} // namespace NNet::NUtils
