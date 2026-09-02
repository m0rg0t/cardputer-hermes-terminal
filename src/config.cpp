#include "hermes_terminal/config.h"
#include "hermes_terminal/ui_rules.h"

#include <SD.h>

namespace hermes_terminal {
namespace {

String trimValue(String value)
{
    value.trim();
    if (value.length() >= 2 && value[0] == '"' &&
        value[value.length() - 1] == '"') {
        value = value.substring(1, value.length() - 1);
        value.replace("\\\"", "\"");
        value.replace("\\\\", "\\");
    }
    return value;
}

bool parseBool(String value, bool fallback)
{
    value.toLowerCase();
    if (value == "true" || value == "yes" || value == "1" || value == "on") {
        return true;
    }
    if (value == "false" || value == "no" || value == "0" || value == "off") {
        return false;
    }
    return fallback;
}

}  // namespace

bool Config::valid() const
{
    const bool passwordLogin = loginUsername.length() > 0 &&
                               loginPassword.length() > 0;
    return wifiSsid.length() > 0 && baseUrl.startsWith("https://") &&
           (sessionToken.length() >= 16 || sessionCookie.length() >= 16 ||
            passwordLogin);
}

bool loadConfig(const char* path, Config& config, String& error)
{
    error = "";
    File file = SD.open(path, FILE_READ);
    if (!file) {
        error = String("Missing ") + path;
        return false;
    }

    Config parsed;
    bool firstLine = true;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (firstLine) {
            // Editors on Windows and macOS may prepend a UTF-8 byte order
            // mark, which would otherwise hide the first key.
            line.remove(0, utf8BomLength(line.c_str(), line.length()));
            firstLine = false;
        }
        line.trim();
        if (line.length() == 0 || line[0] == '#' || line[0] == ';') {
            continue;
        }
        const int separator = line.indexOf('=');
        if (separator <= 0) {
            continue;
        }
        String key = line.substring(0, separator);
        key.trim();
        key.toLowerCase();
        const String value = trimValue(line.substring(separator + 1));
        if (key == "wifi_ssid") parsed.wifiSsid = value;
        else if (key == "wifi_password") parsed.wifiPassword = value;
        else if (key == "hermes_base_url") parsed.baseUrl = value;
        else if (key == "hermes_session_token") parsed.sessionToken = value;
        else if (key == "hermes_session_cookie") parsed.sessionCookie = value;
        else if (key == "hermes_login_username") parsed.loginUsername = value;
        else if (key == "hermes_login_password") parsed.loginPassword = value;
        else if (key == "hermes_profile") parsed.profile = value;
        else if (key == "hermes_cwd") parsed.workingDirectory = value;
        else if (key == "hermes_ca_path") parsed.caPath = value;
        else if (key == "hostname") parsed.hostname = value;
        else if (key == "web_admin_username") parsed.webAdminUsername = value;
        else if (key == "web_admin") parsed.webAdmin = parseBool(value, false);
        else if (key == "web_admin_token") parsed.webAdminToken = value;
        else if (key == "tts_volume") {
            const int volume = value.toInt();
            parsed.ttsVolume = constrain(volume, 0, 255);
        } else if (key == "screen_sleep_seconds") {
            const int seconds = value.toInt();
            parsed.screenSleepSeconds = constrain(seconds, 0, 3600);
        } else if (key == "screen_sleep_brightness") {
            const int brightness = value.toInt();
            parsed.screenSleepBrightness = constrain(brightness, 8, 255);
        }
    }
    file.close();
    parsed.baseUrl.trim();
    while (parsed.baseUrl.endsWith("/")) {
        parsed.baseUrl.remove(parsed.baseUrl.length() - 1);
    }
    const bool hasLoginUsername = parsed.loginUsername.length() > 0;
    const bool hasLoginPassword = parsed.loginPassword.length() > 0;
    if (hasLoginUsername != hasLoginPassword) {
        error = "Set both Hermes login username and password";
        return false;
    }
    const int authModes = (parsed.sessionToken.length() ? 1 : 0) +
                          (parsed.sessionCookie.length() ? 1 : 0) +
                          (hasLoginUsername ? 1 : 0);
    if (authModes != 1) {
        error = "Set exactly one Hermes auth mode";
        return false;
    }
    if (!parsed.valid()) {
        error = "HERMES.CFG incomplete or non-HTTPS URL";
        return false;
    }
    if (parsed.webAdmin && parsed.webAdminToken.length() < 16) {
        error = "web_admin_token must be at least 16 characters";
        return false;
    }
    if (parsed.webAdmin && parsed.webAdminUsername.length() == 0) {
        error = "web_admin_username must not be empty";
        return false;
    }
    config = parsed;
    return true;
}

}  // namespace hermes_terminal
