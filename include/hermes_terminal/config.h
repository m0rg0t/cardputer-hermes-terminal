#pragma once

#include <Arduino.h>

namespace hermes_terminal {

struct Config {
    String wifiSsid;
    String wifiPassword;
    String baseUrl;
    String sessionToken;
    String sessionCookie;
    String loginUsername;
    String loginPassword;
    String profile;
    String workingDirectory;
    String caPath = "/HERMES_CA.PEM";
    String hostname = "hermes-terminal";
    String webAdminUsername = "hermes";
    String webAdminToken;
    bool webAdmin = false;
    std::uint8_t ttsVolume = 180;
    std::uint16_t screenSleepSeconds = 60;
    std::uint8_t screenSleepBrightness = 48;

    bool valid() const;
};

bool loadConfig(const char* path, Config& config, String& error);

}  // namespace hermes_terminal
