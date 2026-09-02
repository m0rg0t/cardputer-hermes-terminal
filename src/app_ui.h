#pragma once

// Private UI surface shared by app.cpp and app_draw.cpp. The drawing unit
// compiles unchanged in the desktop preview (sim/), so keep it free of
// networking, SD, and audio dependencies.

#include <Arduino.h>
#include <M5Cardputer.h>

namespace hermes_terminal {

constexpr unsigned long kMaxVoiceMs = 30000;
constexpr const char* kFirmwareBuild = "2026.09";
// Warm monochrome electronics palette with a single red hardware-style accent.
constexpr std::uint16_t kUiBg = 0x0862;
constexpr std::uint16_t kUiPanel = 0x18E3;
constexpr std::uint16_t kUiInk = 0xE71A;
constexpr std::uint16_t kUiMuted = 0x8C50;
constexpr std::uint16_t kUiRule = 0x39C6;
constexpr std::uint16_t kUiRed = 0xE988;
constexpr std::uint16_t kUiRedDark = 0x58E4;

String singleLine(String value);
const char* activityLabel(const String& status);
M5Canvas* fullScreenCanvas();

}  // namespace hermes_terminal
