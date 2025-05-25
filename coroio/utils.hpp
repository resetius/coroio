#pragma once

#include <string>

namespace NNet {

namespace NUtils {

std::string Base64Encode(const unsigned char* data, size_t dataLen);
void SHA1Digest(const unsigned char* data, size_t dataLen, unsigned char* output);

} // namespace NUtils

} // namespace NNet
