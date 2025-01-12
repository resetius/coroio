#include "ws.hpp"

#include <openssl/evp.h>

namespace NNet
{

std::string GenerateWebSocketKey(std::random_device& rd) {
    std::vector<uint8_t> randomBytes(16);
    std::generate(randomBytes.begin(), randomBytes.end(), std::ref(rd));

    std::string base64;
    base64.resize(4 * ((randomBytes.size() + 2) / 3));
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&base64[0]), randomBytes.data(), randomBytes.size());

    return base64;
}

} // namespace NNet
