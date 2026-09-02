#include <cassert>
#include <cstdint>

#include "hermes_terminal/ws_frame_rules.h"

using hermes_terminal::linkTimedOut;
using hermes_terminal::nextReconnectDelayMs;
using hermes_terminal::ServerFrameError;
using hermes_terminal::validateServerFrame;

int main()
{
    assert(validateServerFrame(0x81, 0x05, 5) == ServerFrameError::kNone);
    assert(validateServerFrame(0x89, 0x7D, 125) == ServerFrameError::kNone);
    assert(validateServerFrame(0x88, 0x00, 0) == ServerFrameError::kNone);
    assert(validateServerFrame(0x81, 0x85, 5) == ServerFrameError::kMasked);
    assert(validateServerFrame(0x01, 0x05, 5) ==
           ServerFrameError::kFragmented);
    assert(validateServerFrame(0xC1, 0x05, 5) ==
           ServerFrameError::kReservedBits);
    assert(validateServerFrame(0x80, 0x00, 0) ==
           ServerFrameError::kContinuation);
    assert(validateServerFrame(0x89, 0x7E, 126) ==
           ServerFrameError::kControlTooLarge);
    assert(validateServerFrame(0x88, 0x01, 1) ==
           ServerFrameError::kInvalidCloseLength);
    assert(validateServerFrame(0x82, 0x02, 2) ==
           ServerFrameError::kUnknownOpcode);
    // A silent link is declared dead only after two missed pings plus grace,
    // and the unsigned subtraction survives millis() wrapping.
    assert(!linkTimedOut(60000, 0, 30000));
    assert(!linkTimedOut(64999, 0, 30000));
    assert(linkTimedOut(65000, 0, 30000));
    assert(!linkTimedOut(10000, 0xFFFFF000U, 30000));
    assert(linkTimedOut(70000 - 4096, 0xFFFFF000U, 30000));
    // Reconnect delay doubles from the base and is capped at one minute.
    assert(nextReconnectDelayMs(5000) == 10000);
    assert(nextReconnectDelayMs(40000) == 60000);
    assert(nextReconnectDelayMs(60000) == 60000);
    assert(nextReconnectDelayMs(0) == 5000);
    return 0;
}
