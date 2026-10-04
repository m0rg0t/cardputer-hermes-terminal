#pragma once

#include <cstdint>

namespace hermes_terminal {

// For scheduled waits shorter than half the uint32_t clock range, unsigned
// subtraction preserves ordering when the ESP32 millis() clock wraps.
inline bool timeReached(std::uint32_t now, std::uint32_t deadline)
{
    return now - deadline < 0x80000000U;
}

inline bool withinTimeout(std::uint32_t now, std::uint32_t started,
                          std::uint32_t timeout)
{
    return now - started < timeout;
}

}  // namespace hermes_terminal
