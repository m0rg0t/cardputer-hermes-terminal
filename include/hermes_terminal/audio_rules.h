#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hermes_terminal {

// Leave the reader immediately after the key. In particular, do not consume
// the following colon: compact FastAPI responses have no intervening space.
template <typename Reader>
bool findAudioDataUrl(Reader& body)
{
    constexpr char token[] = "\"data_url\"";
    std::size_t matched = 0;
    while (matched < sizeof(token) - 1) {
        const int value = body.read();
        if (value < 0) return false;
        matched = value == token[matched] ? matched + 1
                                         : (value == token[0] ? 1 : 0);
    }
    return true;
}

struct PcmWavHeader {
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint32_t dataBytes = 0;
};

inline std::uint16_t audioRead16(const std::uint8_t* bytes)
{
    return bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

inline std::uint32_t audioRead32(const std::uint8_t* bytes)
{
    return audioRead16(bytes) |
           (static_cast<std::uint32_t>(audioRead16(bytes + 2)) << 16);
}

// Validate chunk bounds before seeking, including odd-byte padding. Failed
// seeks and overflowing chunk sizes must never loop or play partial audio.
// On success the file is positioned at the first interleaved PCM sample.
template <typename File>
bool readPcmWavHeader(File& file, PcmWavHeader& header)
{
    header = PcmWavHeader();
    std::uint8_t bytes[16];
    if (file.read(bytes, 12) != 12 || std::memcmp(bytes, "RIFF", 4) ||
        std::memcmp(bytes + 8, "WAVE", 4)) return false;
    bool haveFormat = false;
    while (file.position() < file.size()) {
        if (file.read(bytes, 8) != 8) return false;
        const std::uint32_t size = audioRead32(bytes + 4);
        const std::size_t start = file.position();
        const std::size_t remaining = file.size() - start;
        if (size > remaining) return false;
        if (!std::memcmp(bytes, "data", 4)) {
            if (!haveFormat || !size || size % (header.channels * 2))
                return false;
            header.dataBytes = size;
            return true;
        }
        if (!std::memcmp(bytes, "fmt ", 4)) {
            if (size < 16 || file.read(bytes, 16) != 16) return false;
            header.channels = audioRead16(bytes + 2);
            header.rate = audioRead32(bytes + 4);
            if (audioRead16(bytes) != 1 ||
                (header.channels != 1 && header.channels != 2) ||
                !header.rate || audioRead16(bytes + 12) != header.channels * 2 ||
                audioRead16(bytes + 14) != 16) return false;
            haveFormat = true;
        }
        if ((size & 1U) && size == remaining) return false;
        if (!file.seek(start + size + (size & 1U))) return false;
    }
    return false;
}

}  // namespace hermes_terminal
