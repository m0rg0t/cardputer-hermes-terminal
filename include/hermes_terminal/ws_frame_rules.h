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

constexpr unsigned long kReconnectBaseDelayMs = 5000;
constexpr unsigned long kReconnectMaxDelayMs = 60000;
constexpr unsigned long kLinkGraceMs = 5000;

// A WebSocket is considered dead when nothing (data or pong) has arrived for
// two ping intervals plus a grace period. Uses wrap-safe unsigned arithmetic.
inline bool linkTimedOut(std::uint32_t nowMs, std::uint32_t lastReceiveMs,
                         std::uint32_t pingIntervalMs)
{
    const std::uint32_t silence = nowMs - lastReceiveMs;
    return silence >= 2 * pingIntervalMs + kLinkGraceMs;
}

// Exponential reconnect backoff: each failed attempt doubles the wait so an
// unreachable gateway does not block the main loop with a blocking auth
// round-trip every five seconds.
inline unsigned long nextReconnectDelayMs(unsigned long currentMs)
{
    if (currentMs < kReconnectBaseDelayMs) return kReconnectBaseDelayMs;
    const unsigned long doubled = currentMs * 2;
    return doubled > kReconnectMaxDelayMs ? kReconnectMaxDelayMs : doubled;
}

}  // namespace hermes_terminal
