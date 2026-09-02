#pragma once

#include <Arduino.h>
#include <vector>

namespace hermes_terminal {

struct WifiCredential {
    String ssid;
    String password;
};

struct Config {
    String wifiSsid;
    String wifiPassword;
    // Additional networks from wifi_ssid_2 / wifi_password_2 .. _9.
    std::vector<WifiCredential> wifiExtra;
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
