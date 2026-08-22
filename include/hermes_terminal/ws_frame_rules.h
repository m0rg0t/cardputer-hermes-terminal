#pragma once

#include <cstdint>

namespace hermes_terminal {

enum class ServerFrameError : std::uint8_t {
    kNone,
    kReservedBits,
    kFragmented,
    kContinuation,
    kMasked,
    kControlTooLarge,
    kInvalidCloseLength,
    kUnknownOpcode,
};

inline ServerFrameError validateServerFrame(std::uint8_t first,
                                             std::uint8_t second,
                                             std::uint64_t length)
{
    const std::uint8_t opcode = first & 0x0F;
    if ((first & 0x70) != 0) return ServerFrameError::kReservedBits;
    if ((first & 0x80) == 0) return ServerFrameError::kFragmented;
    if (opcode == 0) return ServerFrameError::kContinuation;
    if ((second & 0x80) != 0) return ServerFrameError::kMasked;
    if (opcode >= 0x8 && length > 125) {
        return ServerFrameError::kControlTooLarge;
    }
    if (opcode == 0x8 && length == 1) {
        return ServerFrameError::kInvalidCloseLength;
    }
    if (opcode != 0x1 && opcode != 0x8 && opcode != 0x9 && opcode != 0xA) {
        return ServerFrameError::kUnknownOpcode;
    }
    return ServerFrameError::kNone;
}

}  // namespace hermes_terminal
