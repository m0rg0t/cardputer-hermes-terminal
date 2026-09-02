#pragma once

#include <cstddef>
#include <cstdint>

namespace hermes_terminal {

constexpr std::uint32_t kCacheSchemaVersion = 1;
constexpr std::size_t kCacheRamWindowBytes = 3072;

inline std::uint32_t cacheCrc32Update(std::uint32_t state,
                                      const std::uint8_t* data,
                                      std::size_t length)
{
    std::uint32_t crc = state;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U &
                  static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U)));
        }
    }
    return crc;
}

inline std::uint32_t cacheCrc32(const std::uint8_t* data, std::size_t length)
{
    return ~cacheCrc32Update(0xFFFFFFFFU, data, length);
}

inline bool cacheSchemaCompatible(std::uint32_t version)
{
    return version == kCacheSchemaVersion;
}

inline std::size_t utf8SafeStart(const char* data, std::size_t length,
                                 std::size_t requested)
{
    if (requested >= length) return length;
    std::size_t start = requested;
    while (start < length &&
           (static_cast<unsigned char>(data[start]) & 0xC0U) == 0x80U) {
        ++start;
    }
    return start;
}

}  // namespace hermes_terminal
