#include "hermes_terminal/app.h"

#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "hermes_terminal/hermes_states.h"
#include "hermes_terminal/stream_text_rules.h"

namespace hermes_terminal {
namespace {

constexpr std::uint8_t kSdClockPin = 40;
constexpr std::uint8_t kSdMisoPin = 39;
constexpr std::uint8_t kSdMosiPin = 14;
constexpr std::uint8_t kSdChipSelectPin = 12;
constexpr std::uint32_t kSdFrequencyHz = 10000000;
constexpr std::size_t kMaxTimeline = 6000;
constexpr unsigned long kMaxVoiceMs = 30000;
constexpr const char* kVoicePath = "/.HERMES-VOICE.wav";
constexpr const char* kTtsPath = "/.HERMES-TTS.bin";
constexpr const char* kCookiePath = "/.HERMES-COOKIE";
constexpr const char* kCookieTempPath = "/.HERMES-COOKIE.tmp";
constexpr const char* kCookieBackupPath = "/.HERMES-COOKIE.bak";
constexpr const char* kUiSettingsPath = "/.HERMES-UI.CFG";
constexpr const char* kUiSettingsTempPath = "/.HERMES-UI.tmp";
constexpr const char* kUiSettingsBackupPath = "/.HERMES-UI.bak";
constexpr std::size_t kMaxTtsText = 12000;
constexpr const char* kInterimBoundary = "\n[INTERIM]\nHERMES: ";
constexpr unsigned long kSleepFrameIntervalMs = 125;
// Warm monochrome electronics palette with a single red hardware-style accent.
constexpr std::uint16_t kUiBg = 0x0862;
constexpr std::uint16_t kUiPanel = 0x18E3;
constexpr std::uint16_t kUiInk = 0xE71A;
constexpr std::uint16_t kUiMuted = 0x8C50;
constexpr std::uint16_t kUiRule = 0x39C6;
constexpr std::uint16_t kUiRed = 0xE988;
constexpr std::uint16_t kUiRedDark = 0x58E4;

String jsonText(JsonVariantConst value)
{
    if (value.is<const char*>()) return String(value.as<const char*>());
    String text;
    serializeJson(value, text);
    return text;
}

String normalizedMdnsName(String value)
{
    value.trim();
    value.toLowerCase();
    if (value.endsWith(".local")) value.remove(value.length() - 6);
    String result;
    result.reserve(value.length());
    for (std::size_t index = 0; index < value.length() && result.length() < 63;
         ++index) {
        const char c = value[index];
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-';
        if (valid) result += c;
        else if (result.length() && result[result.length() - 1] != '-') result += '-';
    }
    while (result.startsWith("-")) result.remove(0, 1);
    while (result.endsWith("-")) result.remove(result.length() - 1);
    return result.length() ? result : String("hermes-terminal");
}

String singleLine(String value)
{
    value.trim();
    String result;
    result.reserve(value.length());
    bool pendingSpace = false;
    for (std::size_t index = 0; index < value.length(); ++index) {
        const char character = value[index];
        if (character == '\n' || character == '\r' || character == '\t' ||
            character == ' ') {
            pendingSpace = result.length() > 0;
            continue;
        }
        if (pendingSpace) {
            result += ' ';
            pendingSpace = false;
        }
        result += character;
    }
    return result;
}

template <typename Surface>
void drawBackgroundAccents(Surface& display)
{
    // Static calibration rail: visible enough to add hardware character, but
    // kept outside the 8 px text cells and session rows.
    display.drawFastHLine(4, 28, 232, kUiRule);
    display.fillRect(220, 27, 6, 2, kUiRed);
    display.drawPixel(230, 27, kUiMuted);
    display.drawPixel(234, 27, kUiMuted);
}

template <typename Surface>
void drawPocketHeader(Surface& display, const char* section, bool connected)
{
    display.setTextWrap(false);
    display.setTextFont(1);
    display.setTextColor(kUiInk, kUiBg);
    display.setCursor(4, 3);
    display.printf("HERMES // %s", section);
    display.setTextColor(kUiMuted, kUiBg);
    display.setCursor(199, 3);
    display.print("LINK");
    display.fillCircle(233, 6, 3, connected ? TFT_GREEN : TFT_ORANGE);
    display.drawFastHLine(0, 14, display.width(), kUiRed);
}

template <typename Surface>
void drawPocketFooter(Surface& display, const char* text)
{
    display.drawFastHLine(0, 120, display.width(), kUiRule);
    display.setTextColor(kUiMuted, kUiBg);
    display.setCursor(4, 125);
    display.print(text);
}

void drawHermesBadge(M5Canvas& canvas, const std::uint8_t* pixels,
                     int originX, int originY)
{
    const std::uint16_t colors[] = {kUiBg, TFT_BLACK, kUiMuted, kUiInk};
    int pixel = 0;
    constexpr std::size_t kFrameBytes =
        kHermesBadgeWidth * kHermesBadgeHeight / 4;
    for (std::size_t index = 0; index < kFrameBytes; ++index) {
        const std::uint8_t packed = pixels[index];
        for (int shift = 6; shift >= 0; shift -= 2, ++pixel) {
            const int x = pixel % kHermesBadgeWidth;
            const int y = pixel / kHermesBadgeWidth;
            canvas.drawPixel(originX + x, originY + y,
                             colors[(packed >> shift) & 0x03]);
        }
    }
}

const char* activityLabel(const String& status)
{
    if (status.indexOf("APPROVAL") >= 0 || status.indexOf("QUESTION") >= 0)
        return "WAIT";
    if (status.indexOf("TOOL") >= 0 || status.indexOf("SUBAGENT") >= 0)
        return "TOOL";
    if (status.indexOf("THINK") >= 0 || status.indexOf("RESPOND") >= 0 ||
        status.indexOf("STARTING") >= 0)
        return "WORK";
    if (status.indexOf("ERROR") >= 0 || status.indexOf("FAILED") >= 0)
        return "ERR";
    return "READY";
}

M5Canvas* fullScreenCanvas()
{
    static M5Canvas canvas(&M5Cardputer.Display);
    static bool attempted = false;
    static bool ready = false;
    if (!attempted) {
        attempted = true;
        canvas.setColorDepth(16);
        ready = canvas.createSprite(M5Cardputer.Display.width(),
                                    M5Cardputer.Display.height()) != nullptr;
    }
    return ready ? &canvas : nullptr;
}

}  // namespace

void App::begin()
{
    Serial.begin(115200);
    auto board = M5.config();
    board.internal_mic = true;
    board.internal_spk = true;
    M5Cardputer.begin(board, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(awakeBrightness_);
    M5Cardputer.Display.setTextWrap(false);
    M5Cardputer.BtnA.setHoldThresh(700);
    // Reserve the single full-screen framebuffer before Wi-Fi/TLS and audio
    // allocations can fragment the heap. Recording and sleep mode share it.
    (void)fullScreenCanvas();
    if (M5.getBoard() != m5::board_t::board_M5CardputerADV) {
        status_ = "CARDPUTER ADV REQUIRED";
        draw();
        return;
    }
    if (!mountSd()) {
        status_ = "SD CARD REQUIRED";
        draw();
        return;
    }
    String error;
    if (!loadConfig("/HERMES.CFG", config_, error)) {
        status_ = error;
        draw();
        return;
    }
    loadUiSettings();
    M5Cardputer.Display.setBrightness(awakeBrightness_);
    lastInputMs_ = millis();
    loadAuthCookie();
    if (!loadCa()) {
        status_ = "HERMES_CA.PEM REQUIRED";
        draw();
        return;
    }
    startWifi();
    hermes_.begin(config_, caPem_, *this);
    audioClient_.begin(config_, caPem_);
    String cueError;
    (void)audioClient_.playUiCue(false, cueError);
    if (!webAdmin_.begin(config_, *this)) {
        status_ = "WEB ADMIN CONFIG INVALID";
    }
    status_ = "CONNECTING";
    draw();
}

bool App::mountSd()
{
    SPI.begin(kSdClockPin, kSdMisoPin, kSdMosiPin, kSdChipSelectPin);
    const bool mounted = SD.begin(kSdChipSelectPin, SPI, kSdFrequencyHz) &&
                         SD.cardType() != CARD_NONE;
    if (mounted) {
        // A reset during capture, synthesis, or playback must not make the
        // previous transient audio persist into the next boot.
        SD.remove(kVoicePath);
        SD.remove(kTtsPath);
        if (!SD.exists(kCookiePath)) {
            // Prefer the last known-good value. A .tmp file may be a partial
            // write, whereas .bak was already the active cookie file.
            if (SD.exists(kCookieBackupPath)) {
                SD.rename(kCookieBackupPath, kCookiePath);
            } else if (SD.exists(kCookieTempPath)) {
                SD.rename(kCookieTempPath, kCookiePath);
            }
        }
        if (SD.exists(kCookiePath)) {
            SD.remove(kCookieTempPath);
            SD.remove(kCookieBackupPath);
        }
    }
    return mounted;
}

bool App::loadCa()
{
    File file = SD.open(config_.caPath, FILE_READ);
    if (!file) return false;
    caPem_ = file.readString();
    file.close();
    return caPem_.indexOf("BEGIN CERTIFICATE") >= 0;
}

void App::loadUiSettings()
{
    if (!SD.exists(kUiSettingsPath)) {
        if (SD.exists(kUiSettingsBackupPath))
            SD.rename(kUiSettingsBackupPath, kUiSettingsPath);
        else if (SD.exists(kUiSettingsTempPath))
            SD.rename(kUiSettingsTempPath, kUiSettingsPath);
    }
    File file = SD.open(kUiSettingsPath, FILE_READ);
    if (!file) return;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        const int separator = line.indexOf('=');
        if (separator <= 0) continue;
        String key = line.substring(0, separator);
        String value = line.substring(separator + 1);
        key.trim();
        value.trim();
        if (key == "awake") awakeBrightness_ = constrain(value.toInt(), 30, 255);
        else if (key == "sleep") config_.screenSleepSeconds = constrain(value.toInt(), 0, 600);
        else if (key == "dim") config_.screenSleepBrightness = constrain(value.toInt(), 8, 120);
        else if (key == "tts") config_.ttsVolume = constrain(value.toInt(), 0, 255);
        else if (key == "motion") sleepMotion_ = constrain(value.toInt(), 0, 2);
        else if (key == "alerts") alertsEnabled_ = value == "1";
    }
    file.close();
    SD.remove(kUiSettingsTempPath);
    SD.remove(kUiSettingsBackupPath);
}

bool App::saveUiSettings()
{
    SD.remove(kUiSettingsTempPath);
    File file = SD.open(kUiSettingsTempPath, FILE_WRITE);
    if (!file) return false;
    file.printf("awake=%u\nsleep=%u\ndim=%u\ntts=%u\nmotion=%u\nalerts=%u\n",
                awakeBrightness_, config_.screenSleepSeconds,
                config_.screenSleepBrightness, config_.ttsVolume,
                sleepMotion_, alertsEnabled_ ? 1 : 0);
    file.flush();
    file.close();
    SD.remove(kUiSettingsBackupPath);
    if (SD.exists(kUiSettingsPath) &&
        !SD.rename(kUiSettingsPath, kUiSettingsBackupPath)) {
        SD.remove(kUiSettingsTempPath);
        return false;
    }
    if (!SD.rename(kUiSettingsTempPath, kUiSettingsPath)) {
        if (SD.exists(kUiSettingsBackupPath))
            SD.rename(kUiSettingsBackupPath, kUiSettingsPath);
        return false;
    }
    SD.remove(kUiSettingsBackupPath);
    uiSettingsDirty_ = false;
    return true;
}

void App::adjustUiSetting(int delta)
{
    switch (settingRow_) {
        case 0:
            awakeBrightness_ = constrain(static_cast<int>(awakeBrightness_) +
                                         delta * 15, 30, 255);
            M5Cardputer.Display.setBrightness(awakeBrightness_);
            break;
        case 1:
            if (delta > 0)
                config_.screenSleepSeconds = config_.screenSleepSeconds == 0
                                                ? 30
                                                : min<int>(600, config_.screenSleepSeconds + 30);
            else
                config_.screenSleepSeconds = config_.screenSleepSeconds <= 30
                                                ? 0
                                                : config_.screenSleepSeconds - 30;
            break;
        case 2:
            config_.screenSleepBrightness = constrain(
                static_cast<int>(config_.screenSleepBrightness) + delta * 8,
                8, 120);
            break;
        case 3:
            config_.ttsVolume = constrain(static_cast<int>(config_.ttsVolume) +
                                           delta * 15, 0, 255);
            audioClient_.setTtsVolume(config_.ttsVolume);
            break;
        case 4:
            sleepMotion_ = constrain(static_cast<int>(sleepMotion_) + delta,
                                     0, 2);
            break;
        case 5:
            alertsEnabled_ = !alertsEnabled_;
            break;
    }
    uiSettingsDirty_ = true;
}

void App::loadAuthCookie()
{
    // Password-login mode persists the endpoint-bound session so normal
    // reconnects can mint tickets without sending the password every time.
    if (config_.sessionToken.length()) return;
    File file = SD.open(kCookiePath, FILE_READ);
    if (!file) return;
    String endpoint = file.readStringUntil('\n');
    endpoint.trim();
    String cookie = file.readString();
    cookie.trim();
    file.close();
    if (endpoint == config_.baseUrl && cookie.length() >= 16 &&
        cookie.indexOf('=') > 0) {
        config_.sessionCookie = cookie;
    }
}

bool App::saveAuthCookie(const String& cookie)
{
    SD.remove(kCookieTempPath);
    File file = SD.open(kCookieTempPath, FILE_WRITE);
    if (!file) return false;
    const bool written = file.println(config_.baseUrl) > 0 &&
                         file.print(cookie) == cookie.length();
    file.flush();
    file.close();
    if (!written) {
        SD.remove(kCookieTempPath);
        return false;
    }
    SD.remove(kCookieBackupPath);
    if (SD.exists(kCookiePath) &&
        !SD.rename(kCookiePath, kCookieBackupPath)) {
        SD.remove(kCookieTempPath);
        return false;
    }
    if (!SD.rename(kCookieTempPath, kCookiePath)) {
        if (SD.exists(kCookieBackupPath)) {
            SD.rename(kCookieBackupPath, kCookiePath);
        }
        return false;
    }
    SD.remove(kCookieBackupPath);
    return true;
}

void App::startWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.setHostname(config_.hostname.c_str());
    WiFi.begin(config_.wifiSsid.c_str(), config_.wifiPassword.c_str());
}

void App::update()
{
    M5Cardputer.update();
    serviceMdns();
    hermes_.update();
    webAdmin_.update();
    if (!hermes_.connected()) {
        const String diagnostic = hermes_.diagnostic();
        if (diagnostic.length() && diagnostic != "NOT STARTED" &&
            diagnostic != lastHermesDiagnostic_) {
            lastHermesDiagnostic_ = diagnostic;
            status_ = diagnostic;
            dirty_ = true;
        }
    }
    if (voice_.active()) {
        if (!voice_.update()) {
            status_ = voice_.error();
            voice_.cancel();
            if (voiceTest_) {
                voiceTest_ = false;
                helpPage_ = 2;
                screen_ = Screen::kHelpSettings;
            } else {
                screen_ = Screen::kChat;
            }
            dirty_ = true;
        } else if (voice_.elapsedMs() >= kMaxVoiceMs) {
            finishVoice(true);
        } else if (millis() - recordingFrameMs_ >= 100) {
            recordingFrameMs_ = millis();
            dirty_ = true;
        }
    }
    serviceInput();
    serviceScreenSleep();
    if (screen_ == Screen::kSessions && sessions_.empty() &&
        (!hermes_.connected() || sessionsRequestId_) &&
        millis() - recordingFrameMs_ >= 180) {
        recordingFrameMs_ = millis();
        dirty_ = true;
    }
    if (dirty_) draw();
    delay(2);
}

void App::serviceMdns()
{
    const bool shouldRun = WiFi.status() == WL_CONNECTED && webAdmin_.enabled();
    if (!shouldRun) {
        if (mdnsStarted_) {
            MDNS.end();
            mdnsStarted_ = false;
            mdnsName_ = "";
        }
        return;
    }
    if (mdnsStarted_) return;

    mdnsName_ = normalizedMdnsName(config_.hostname);
    if (!MDNS.begin(mdnsName_.c_str())) {
        mdnsName_ = "";
        return;
    }
    MDNS.setInstanceName("Hermes Terminal Admin");
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "path", "/");
    MDNS.addServiceTxt("http", "tcp", "auth", "basic");
    mdnsStarted_ = true;
}

void App::onHermesConnected()
{
    lastHermesDiagnostic_ = hermes_.diagnostic();
    String cueError;
    (void)audioClient_.playUiCue(true, cueError);
    status_ = "HERMES CONNECTED";
    if (activeStoredSessionId_.length() && screen_ != Screen::kSessions) {
        JsonDocument params;
        params["session_id"] = activeStoredSessionId_;
        if (config_.profile.length()) params["profile"] = config_.profile;
        resumeRequestId_ =
            hermes_.request("session.resume", params.as<JsonObjectConst>());
        status_ = "RESTORING SESSION";
    } else {
        requestSessions();
    }
    dirty_ = true;
}

void App::onHermesDisconnected(const String& reason)
{
    lastHermesDiagnostic_ = reason;
    activeSessionId_ = "";
    status_ = reason + " - RETRYING";
    dirty_ = true;
}

void App::onHermesMessage(JsonDocument& message)
{
    JsonObjectConst root = message.as<JsonObjectConst>();
    if (root["method"] == "event") {
        parseEvent(root["params"].as<JsonObjectConst>());
    } else if (!root["id"].isNull()) {
        parseResponse(root);
    }
    dirty_ = true;
}

void App::onHermesAuthCookieUpdated(const String& cookie)
{
    config_.sessionCookie = cookie;
    audioClient_.setSessionCookie(cookie);
    if (!saveAuthCookie(cookie)) status_ = "AUTH COOKIE SAVE FAILED";
}

void App::writeWebStatus(JsonObject output)
{
    output["device"] = config_.hostname;
    output["ip"] = WiFi.localIP().toString();
    output["hermes_connected"] = hermes_.connected();
    output["status"] = status_;
    output["diagnostic"] = hermes_.diagnostic();
    output["auth_mode"] = hermes_.authMode();
    output["auth_configured"] = hermes_.authConfigured();
    output["gateway_auth_mode"] = hermes_.gatewayAuthMode();
    output["gateway_auth_required"] = hermes_.gatewayAuthRequired();
    output["gateway_auth_flows"] = hermes_.gatewayAuthFlows();
    output["session_id"] = activeSessionId_;
    output["stored_session_id"] = activeStoredSessionId_;
    output["session_title"] = activeSessionTitle_;
    output["recording"] = voice_.active();
    output["free_heap"] = ESP.getFreeHeap();
    output["mdns_name"] = mdnsName_;
    output["mdns_url"] = mdnsStarted_ ? String("http://") + mdnsName_ + ".local/" : "";
    output["mdns_http_service"] = mdnsStarted_;
}

bool App::updateWebAuthCookie(const String& cookie)
{
    String value = cookie;
    value.trim();
    // Keep this endpoint deliberately narrow: it accepts a Cookie header,
    // never a password or an arbitrary header blob. Reject control characters
    // before the value reaches HTTPClient or the SD-card runtime file.
    if ((!config_.sessionCookie.length() && !config_.loginUsername.length()) ||
        config_.sessionToken.length() ||
        value.length() < 16 || value.length() > 4096 ||
        value.indexOf('=') <= 0 || value.indexOf('\r') >= 0 ||
        value.indexOf('\n') >= 0) {
        return false;
    }
    if (!saveAuthCookie(value)) {
        status_ = "AUTH COOKIE SAVE FAILED";
        dirty_ = true;
        return false;
    }

    config_.sessionCookie = value;
    audioClient_.setSessionCookie(value);
    hermes_.setSessionCookie(value);
    hermes_.disconnect();
    status_ = "COOKIE SAVED - CONNECTING";
    lastHermesDiagnostic_ = "";
    dirty_ = true;
    return true;
}

bool App::submitWebPrompt(const String& text)
{
    if (!hermes_.connected() || !activeSessionId_.length() || voice_.active()) {
        return false;
    }
    if (!submitText(text)) return false;
    dirty_ = true;
    return true;
}

bool App::interruptWebSession()
{
    if (!hermes_.connected() || !activeSessionId_.length()) return false;
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    return hermes_.request("session.interrupt", params.as<JsonObjectConst>()) != 0;
}

void App::requestSessions()
{
    JsonDocument params;
    params["limit"] = 12;
    if (config_.profile.length()) params["profile"] = config_.profile;
    sessionsRequestId_ = hermes_.request("session.list", params.as<JsonObjectConst>());
    status_ = sessionsRequestId_ ? "LOADING SESSIONS" : "SESSION LIST FAILED";
}

void App::createSession(bool voiceFirst)
{
    pendingVoiceSession_ = voiceFirst;
    if (!hermes_.connected()) {
        pendingVoiceSession_ = false;
        status_ = "HERMES OFFLINE";
        return;
    }
    JsonDocument params;
    if (config_.workingDirectory.length()) params["cwd"] = config_.workingDirectory;
    if (config_.profile.length()) params["profile"] = config_.profile;
    createRequestId_ = hermes_.request("session.create", params.as<JsonObjectConst>());
    if (!createRequestId_) {
        pendingVoiceSession_ = false;
        status_ = "SESSION CREATE FAILED";
        return;
    }
    status_ = voiceFirst ? "CREATING VOICE SESSION" : "CREATING SESSION";
}

void App::openSession()
{
    if (sessions_.empty()) return;
    activeStoredSessionId_ = sessions_[selectedSession_].id;
    activeSessionId_ = "";
    activeSessionTitle_ = sessions_[selectedSession_].title;
    timeline_ = "";
    scroll_ = 0;
    screen_ = Screen::kChat;
    JsonDocument params;
    params["session_id"] = activeStoredSessionId_;
    if (config_.profile.length()) params["profile"] = config_.profile;
    resumeRequestId_ =
        hermes_.request("session.resume", params.as<JsonObjectConst>());
    if (resumeRequestId_) {
        status_ = "RESUMING SESSION";
    } else {
        screen_ = Screen::kSessions;
        activeStoredSessionId_ = "";
        activeSessionTitle_ = "";
        status_ = "SESSION LOAD FAILED";
    }
}

void App::returnToSessions()
{
    const bool cancelledLoad = resumeRequestId_ != 0 &&
                               !activeSessionId_.length();
    if (cancelledLoad) {
        cancelledResumeRequestId_ = resumeRequestId_;
        resumeRequestId_ = 0;
    }
    if (hermes_.connected() && activeSessionId_.length()) {
        JsonDocument params;
        params["session_id"] = activeSessionId_;
        hermes_.request("session.close", params.as<JsonObjectConst>());
    }
    activeSessionId_ = "";
    activeStoredSessionId_ = "";
    activeSessionTitle_ = "";
    currentAssistantText_ = "";
    lastInterimText_ = "";
    reasoningOpen_ = false;
    historyRequestId_ = 0;
    screen_ = Screen::kSessions;
    if (cancelledLoad) status_ = "SESSION LOAD CANCELLED";
    requestSessions();
}

void App::requestHistory()
{
    if (!activeSessionId_.length()) return;
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    params["limit"] = 24;
    historyRequestId_ = hermes_.request("session.history", params.as<JsonObjectConst>());
}

void App::parseResponse(JsonObjectConst root)
{
    const std::uint32_t id = root["id"] | 0U;
    if (id == cancelledResumeRequestId_) {
        cancelledResumeRequestId_ = 0;
        if (root["error"].isNull()) {
            const String runtimeId = root["result"]["session_id"] | "";
            if (runtimeId.length()) {
                JsonDocument params;
                params["session_id"] = runtimeId;
                hermes_.request("session.close", params.as<JsonObjectConst>());
            }
        }
        return;
    }
    if (!root["error"].isNull()) {
        if (id == slashRequestId_) {
            dispatchPendingCommand();
            return;
        }
        if (id == createRequestId_) pendingVoiceSession_ = false;
        if (id == sessionsRequestId_) sessionsRequestId_ = 0;
        if (id == resumeRequestId_) {
            resumeRequestId_ = 0;
            if (pendingVoiceTranscript_.length()) {
                compose_ = pendingVoiceTranscript_;
                composeMode_ = ComposeMode::kPrompt;
                screen_ = Screen::kCompose;
            } else {
                activeStoredSessionId_ = "";
                activeSessionTitle_ = "";
                screen_ = Screen::kSessions;
            }
        }
        status_ = "RPC ERROR " + jsonText(root["error"]);
        return;
    }
    JsonVariantConst result = root["result"];
    if (id == sessionsRequestId_) {
        sessionsRequestId_ = 0;
        sessions_.clear();
        JsonArrayConst items = result["sessions"].as<JsonArrayConst>();
        if (items.isNull()) items = result["data"].as<JsonArrayConst>();
        if (items.isNull() && result.is<JsonArrayConst>()) items = result.as<JsonArrayConst>();
        for (JsonObjectConst item : items) {
            Session session;
            session.id = item["session_id"] | item["id"] | "";
            session.title = item["title"] | item["name"] | "Untitled";
            session.preview = item["preview"] | "";
            session.state = item["status"] | item["state"] | "";
            if (session.id.length()) sessions_.push_back(session);
        }
        selectedSession_ = min(selectedSession_, max(0, static_cast<int>(sessions_.size()) - 1));
        status_ = String(sessions_.size()) + " HERMES SESSIONS";
    } else if (id == createRequestId_) {
        const bool voiceFirst = pendingVoiceSession_;
        pendingVoiceSession_ = false;
        activeSessionId_ = result["session_id"] | result["id"] | "";
        activeStoredSessionId_ =
            result["stored_session_id"] | activeSessionId_;
        activeSessionTitle_ = result["title"] | "New session";
        if (activeSessionId_.length()) {
            timeline_ = "";
            compose_ = "";
            if (voiceFirst) {
                screen_ = Screen::kChat;
                status_ = "STARTING VOICE";
                startVoice();
            } else {
                screen_ = Screen::kCompose;
                status_ = "TYPE FIRST PROMPT";
            }
        } else {
            status_ = "SESSION CREATE INVALID";
        }
    } else if (id == resumeRequestId_) {
        resumeRequestId_ = 0;
        // The durable session id selected from session.list is not necessarily
        // the live runtime id used by prompt.submit after a resume.
        const String runtimeId = result["session_id"] | "";
        if (runtimeId.length()) activeSessionId_ = runtimeId;
        updateUsage(result["info"]);
        JsonObjectConst pendingApproval =
            result["pending_approval"].as<JsonObjectConst>();
        JsonObjectConst pendingClarify =
            result["pending_clarify"].as<JsonObjectConst>();
        if (!pendingApproval.isNull()) {
            interactionType_ = "approval.request";
            interactionId_ = pendingApproval["request_id"] | "";
            interactionPrompt_ = pendingApproval["description"] |
                                 pendingApproval["command"] |
                                 "Pending Hermes approval";
            approvalChoices_ = ",";
            for (const char* choice :
                 pendingApproval["choices"].as<JsonArrayConst>()) {
                approvalChoices_ += String(choice) + ",";
            }
            if (approvalChoices_ == ",") {
                approvalChoices_ = ",once,deny,";
            }
            compose_ = "";
            screen_ = Screen::kInteraction;
            status_ = "RESTORED APPROVAL";
        } else if (!pendingClarify.isNull()) {
            interactionType_ = "clarify.request";
            interactionId_ = pendingClarify["request_id"] | "";
            prepareClarify(pendingClarify);
            compose_ = "";
            screen_ = Screen::kInteraction;
            status_ = "RESTORED QUESTION";
        } else if (pendingVoiceTranscript_.length()) {
            const String transcript = pendingVoiceTranscript_;
            if (submitText(transcript)) {
                compose_ = "";
            }
        } else {
            requestHistory();
        }
    } else if (id == branchRequestId_) {
        activeSessionId_ = result["session_id"] | "";
        activeStoredSessionId_ =
            result["stored_session_id"] | activeSessionId_;
        activeSessionTitle_ = result["title"] | "Branch";
        if (activeSessionId_.length()) {
            requestHistory();
            status_ = "BRANCH CREATED";
        }
    } else if (id == compressRequestId_) {
        status_ = String(result["status"] | "COMPRESSED");
        requestHistory();
    } else if (id == undoRequestId_) {
        status_ = "UNDO REMOVED " + String(result["removed"] | 0);
        requestHistory();
    } else if (id == steerRequestId_) {
        status_ = String("STEER ") + String(result["status"] | "SENT");
    } else if (id == slashRequestId_ || id == commandRequestId_) {
        handleCommandResult(result);
    } else if (id == historyRequestId_) {
        timeline_ = "";
        lastAssistantText_ = "";
        JsonArrayConst messages = result["messages"].as<JsonArrayConst>();
        if (messages.isNull() && result.is<JsonArrayConst>()) messages = result.as<JsonArrayConst>();
        for (JsonObjectConst item : messages) {
            const String role = item["role"] | item["type"] | "";
            String text = item["text"] | item["content"] | "";
            if (!text.length()) text = jsonText(item["content"]);
            if (text.length()) {
                appendTimeline((role == "user" ? "YOU: " : "HERMES: ") +
                               text + "\n\n");
                if (role != "user") {
                    lastAssistantText_ = text.length() > kMaxTtsText
                                             ? text.substring(0, kMaxTtsText)
                                             : text;
                }
            }
        }
        status_ = activeSessionTitle_;
    }
}

void App::parseEvent(JsonObjectConst params)
{
    const String type = params["type"] | "";
    JsonObjectConst payload = params["payload"].as<JsonObjectConst>();
    const String eventSession = params["session_id"] | "";
    if (screen_ == Screen::kSessions && eventSession.length()) return;
    if (eventSession.length() && activeSessionId_.length() &&
        eventSession != activeSessionId_) return;

    if (type == "gateway.ready") {
        status_ = "HERMES READY";
    } else if (type == "message.start") {
        currentAssistantText_ = "";
        lastInterimText_ = "";
        reasoningOpen_ = false;
        status_ = "HERMES STARTING RESPONSE";
    } else if (type == "message.delta") {
        const String delta = payload["text"] | "";
        if (reasoningOpen_) {
            appendTimeline("\nHERMES: ");
            reasoningOpen_ = false;
        }
        appendTimeline(delta);
        if (currentAssistantText_.length() < kMaxTtsText) {
            currentAssistantText_ += delta.substring(
                0, kMaxTtsText - currentAssistantText_.length());
        }
        status_ = "HERMES RESPONDING";
    } else if (type == "message.interim") {
        const String text = payload["text"] | "";
        const bool alreadyStreamed = payload["already_streamed"] | false;
        if (!alreadyStreamed && text.length()) {
            if (!currentAssistantText_.length()) appendTimeline(text);
            else if (text.startsWith(currentAssistantText_)) {
                appendTimeline(text.substring(currentAssistantText_.length()));
            }
        }
        lastInterimText_ = text;
        currentAssistantText_ = "";
        reasoningOpen_ = false;
        appendTimeline(kInterimBoundary);
        status_ = "HERMES USING TOOLS";
    } else if (type == "message.complete") {
        const String completeText = payload["text"] | "";
        updateUsage(payload);
        if (timeline_.endsWith(kInterimBoundary) &&
            !currentAssistantText_.length()) {
            timeline_.remove(timeline_.length() - strlen(kInterimBoundary));
        }
        const CompletionAppendDecision completion = decideCompletionAppend(
            currentAssistantText_.c_str(), currentAssistantText_.length(),
            lastInterimText_.c_str(), lastInterimText_.length(),
            completeText.c_str(), completeText.length());
        if (completion.kind == CompletionAppendKind::kSuffix &&
            completion.offset < completeText.length()) {
            appendTimeline(completeText.substring(completion.offset));
        } else if (completion.kind == CompletionAppendKind::kFullWithLabel) {
            appendTimeline("\nHERMES: " + completeText);
        }
        if (completeText.length()) {
            lastAssistantText_ = completeText.length() > kMaxTtsText
                                     ? completeText.substring(0, kMaxTtsText)
                                     : completeText;
        } else if (currentAssistantText_.length()) {
            lastAssistantText_ = currentAssistantText_;
        }
        currentAssistantText_ = "";
        lastInterimText_ = "";
        reasoningOpen_ = false;
        appendTimeline("\n\n");
        const String completionStatus = payload["status"] | "";
        status_ = completionStatus == "error"
                      ? "HERMES TURN FAILED"
                      : activeSessionTitle_;
    } else if (type == "reasoning.delta" || type == "thinking.delta") {
        const String delta = payload["text"] | "";
        if (!reasoningOpen_) {
            appendTimeline("\n[THINKING] ");
            reasoningOpen_ = true;
        }
        appendTimeline(delta);
        status_ = "HERMES THINKING";
    } else if (type == "reasoning.available") {
        const String text = payload["text"] | "";
        if (text.length()) appendTimeline("\n[REASONING] " + text + "\n");
        status_ = "HERMES REASONING READY";
    } else if (type == "tool.start") {
        reasoningOpen_ = false;
        appendTimeline("\n[TOOL " + String(payload["name"] | "running") + "]\n");
        status_ = "TOOL RUNNING";
    } else if (type == "tool.generating") {
        status_ = "PREPARING " +
                  shortText(payload["name"] | "TOOL", 27);
    } else if (type == "tool.progress") {
        status_ = shortText(payload["preview"] | payload["name"] | "TOOL", 36);
    } else if (type == "tool.complete") {
        appendTimeline("[TOOL DONE]\n");
    } else if (type == "subagent.start") {
        appendTimeline("\n[SUBAGENT " +
                       String(payload["goal"] | payload["subagent_id"] | "started") +
                       "]\n");
        status_ = "SUBAGENT RUNNING";
    } else if (type == "subagent.tool") {
        status_ = "SUBAGENT " + String(payload["tool_name"] | "TOOL");
    } else if (type == "subagent.thinking" ||
               type == "subagent.progress" ||
               type == "subagent.spawn_requested") {
        status_ = "SUBAGENT " + shortText(
            payload["text"] | payload["status"] | type, 27);
    } else if (type == "subagent.complete") {
        appendTimeline("[SUBAGENT COMPLETE]\n");
    } else if (type == "status.update") {
        status_ = shortText(payload["text"] | payload["kind"] | "HERMES", 38);
    } else if (type == "session.info") {
        updateUsage(payload);
        const String model = payload["model"] | "";
        if (model.length()) status_ = shortText(model, 38);
    } else if (type == "session.usage") {
        updateUsage(payload);
    } else if (type == "session.resume_progress" && resumeRequestId_) {
        const String phase = payload["phase"] | "session";
        const String progress = payload["status"] | "working";
        status_ = shortText("RESUME " + phase + " " + progress, 38);
    } else if (type == "sessions.changed" && screen_ == Screen::kSessions) {
        requestSessions();
    } else if (type == "approval.request" || type == "clarify.request" ||
               type == "sudo.request" || type == "secret.request") {
        interactionType_ = type;
        interactionId_ = payload["request_id"] | "";
        interactionPrompt_ = payload["question"] | payload["description"] |
                             payload["prompt"] | payload["reason"] |
                             payload["command"] | payload["env_var"] |
                             payload["tool_name"] | type;
        if (type == "approval.request") {
            const String command = payload["command"] | "";
            if (command.length() && interactionPrompt_ != command) {
                interactionPrompt_ += "\n$ " + command;
            }
            approvalChoices_ = ",";
            for (const char* choice : payload["choices"].as<JsonArrayConst>()) {
                approvalChoices_ += String(choice) + ",";
            }
            if (approvalChoices_ == ",") {
                approvalChoices_ = ",once,deny,";
            }
        } else if (type == "clarify.request") {
            prepareClarify(payload);
        }
        compose_ = "";
        screen_ = Screen::kInteraction;
        status_ = type;
        if (alertsEnabled_) {
            String ignored;
            (void)audioClient_.testSpeaker(ignored);
        }
    } else if (type == "error") {
        status_ = "HERMES ERROR";
        appendTimeline("\n[ERROR] " + String(payload["message"] | "unknown") + "\n");
    }
}

void App::updateUsage(JsonVariantConst value)
{
    JsonVariantConst usage = value["usage"];
    if (usage.isNull()) usage = value;
    if (!usage.is<JsonObjectConst>()) return;
    if (usage["total"].isNull() && usage["total_tokens"].isNull()) return;
    std::uint32_t total = usage["total"] | 0U;
    if (!total) total = usage["total_tokens"] | 0U;
    if (total >= 1000000U) {
        usageText_ = String(total / 1000000U) + "m";
    } else if (total >= 1000U) {
        usageText_ = String(total / 1000U) + "k";
    } else {
        usageText_ = String(total) + "t";
    }
}

void App::appendTimeline(const String& text)
{
    timeline_ += text;
    if (timeline_.length() > kMaxTimeline) {
        timeline_.remove(0, timeline_.length() - kMaxTimeline);
    }
    scroll_ = 32767;
}

void App::serviceInput()
{
    if (M5Cardputer.BtnA.wasHold()) {
        if (voice_.active()) return;
        lastInputMs_ = millis();
        if (screenSleep_) exitScreenSleep();
        if (screen_ == Screen::kHelpSettings) {
            if (uiSettingsDirty_ && !saveUiSettings())
                status_ = "SETTINGS SAVE FAILED";
            screen_ = helpReturnScreen_;
        } else {
            helpReturnScreen_ = screen_;
            helpPage_ = 0;
            screen_ = Screen::kHelpSettings;
        }
        dirty_ = true;
        return;
    }
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    if (screenSleep_) {
        exitScreenSleep();
        return;
    }
    lastInputMs_ = millis();
    const auto& keys = M5Cardputer.Keyboard.keysState();
    char key = '\0';
    if (keys.enter) key = '\n';
    else if (keys.del) key = '\b';
    else if (!keys.word.empty()) key = keys.word[0];

    if ((screen_ == Screen::kSessions || screen_ == Screen::kChat) &&
        (key == 'z' || key == 'Z')) {
        enterScreenSleep();
        return;
    }

    if (screen_ == Screen::kHelpSettings) {
        if (key == '`') {
            if (uiSettingsDirty_ && !saveUiSettings())
                status_ = "SETTINGS SAVE FAILED";
            screen_ = helpReturnScreen_;
        } else if (helpPage_ == 0) {
            if (key == '.' || key == 's' || key == 'S') helpPage_ = 1;
            else if (key == 'd' || key == 'D') helpPage_ = 2;
        } else if (helpPage_ == 2) {
            if (key == 'h' || key == 'H') helpPage_ = 0;
            else if (key == 's' || key == 'S') helpPage_ = 1;
            else if (key == 'r' || key == 'R') {
                hermes_.disconnect();
                status_ = "RECONNECT REQUESTED";
            } else if (key == 'm' || key == 'M') {
                startVoiceTest();
            } else if (key == 'k' || key == 'K') {
                status_ = "SPEAKER TEST";
                dirty_ = true;
                draw();
                String error;
                status_ = audioClient_.testSpeaker(error)
                              ? "SPEAKER TEST COMPLETE" : error;
            }
        } else if (key == 'h' || key == 'H') {
            helpPage_ = 0;
        } else if (key == 'd' || key == 'D') {
            helpPage_ = 2;
        } else if (key == ';') {
            settingRow_ = (settingRow_ + 5) % 6;
        } else if (key == '.') {
            settingRow_ = (settingRow_ + 1) % 6;
        } else if (key == '-' || key == '[' || key == ',') {
            adjustUiSetting(-1);
        } else if (key == '=' || key == ']' || key == '/' || key == '\n') {
            adjustUiSetting(1);
        }
    } else if (screen_ == Screen::kSessions) {
        if (key == ';' && !sessions_.empty()) selectedSession_ = (selectedSession_ + sessions_.size() - 1) % sessions_.size();
        else if (key == '.' && !sessions_.empty()) selectedSession_ = (selectedSession_ + 1) % sessions_.size();
        else if (key == '\n') openSession();
        else if ((key == 'n' || key == 'N') && hermes_.connected() &&
                 !sessionsRequestId_) createSession(false);
        else if ((key == 'v' || key == 'V') && hermes_.connected() &&
                 !sessionsRequestId_) createSession(true);
        else if (key == 'd' || key == 'D') {
            helpReturnScreen_ = screen_;
            helpPage_ = 2;
            screen_ = Screen::kHelpSettings;
        }
        else if (key == 'r' || key == 'R') {
            if (hermes_.connected()) {
                requestSessions();
            } else {
                hermes_.disconnect();
                lastHermesDiagnostic_ = "";
                status_ = "CONNECTING";
            }
        }
    } else if (screen_ == Screen::kChat) {
        if (key == '`') returnToSessions();
        else if (resumeRequestId_ && !activeSessionId_.length()) {
            // Loading accepts ESC only. Keeping this explicit prevents prompt,
            // voice, or session-control actions from racing session.resume.
        }
        else if (key == 't' || key == 'T') {
            screen_ = Screen::kCompose; compose_ = "";
            composeMode_ = ComposeMode::kPrompt;
        }
        else if (key == 'v' || key == 'V') startVoice();
        else if (key == 'p' || key == 'P' || key == 'r' || key == 'R')
            speakLastResponse();
        else if (key == 's' || key == 'S') {
            screen_ = Screen::kCompose; compose_ = "";
            composeMode_ = ComposeMode::kSteer;
            status_ = "STEER CURRENT TURN";
        } else if (key == '/') {
            screen_ = Screen::kCompose; compose_ = "/";
            composeMode_ = ComposeMode::kPrompt;
            status_ = "HERMES COMMAND";
        } else if (key == 'b' || key == 'B') {
            JsonDocument params; params["session_id"] = activeSessionId_;
            branchRequestId_ = hermes_.request("session.branch", params.as<JsonObjectConst>());
            status_ = "CREATING BRANCH";
        } else if (key == 'c' || key == 'C') {
            JsonDocument params; params["session_id"] = activeSessionId_;
            compressRequestId_ = hermes_.request("session.compress", params.as<JsonObjectConst>());
            status_ = "COMPRESSING";
        } else if (key == 'u' || key == 'U') {
            JsonDocument params; params["session_id"] = activeSessionId_;
            undoRequestId_ = hermes_.request("session.undo", params.as<JsonObjectConst>());
            status_ = "UNDOING LAST TURN";
        }
        else if (key == 'x' || key == 'X') {
            JsonDocument params; params["session_id"] = activeSessionId_;
            hermes_.request("session.interrupt", params.as<JsonObjectConst>());
        } else if (key == ';') scroll_ = max(0, scroll_ - 3);
        else if (key == '.') scroll_ += 3;
    } else if (screen_ == Screen::kCompose) {
        if (key == '`') screen_ = Screen::kChat;
        else if ((key == 'v' || key == 'V') && !compose_.length()) startVoice();
        else if (key == '\n') submitCompose();
        else if (key == '\b' && compose_.length()) compose_.remove(compose_.length() - 1);
        else if (!keys.word.empty() && compose_.length() < 4000) {
            for (char character : keys.word) compose_ += character;
        }
    } else if (screen_ == Screen::kInteraction) {
        if (interactionType_ == "approval.request") {
            if ((key == 'o' || key == 'O' || key == '\n') &&
                approvalChoices_.indexOf(",once,") >= 0) respondInteraction("once");
            else if ((key == 's' || key == 'S') &&
                     approvalChoices_.indexOf(",session,") >= 0) respondInteraction("session");
            else if ((key == 'a' || key == 'A') &&
                     approvalChoices_.indexOf(",always,") >= 0) respondInteraction("always");
            else if ((key == 'd' || key == 'D' || key == '`') &&
                     approvalChoices_.indexOf(",deny,") >= 0) respondInteraction("deny");
        } else {
            if (key == '\n') respondInteraction(compose_);
            else if (key == '`') respondInteraction("");
            else if (key == '\b' && compose_.length()) compose_.remove(compose_.length() - 1);
            else if (!keys.word.empty() && compose_.length() < 1000) {
                for (char character : keys.word) compose_ += character;
            }
        }
    } else if (screen_ == Screen::kRecording) {
        if (key == '\n' || key == 'v' || key == 'V') finishVoice(true);
        else if (key == '`') finishVoice(false);
    }
    dirty_ = true;
}

bool App::canScreenSleep() const
{
    return config_.screenSleepSeconds > 0 && !voice_.active() &&
           (screen_ == Screen::kSessions || screen_ == Screen::kChat);
}

void App::enterScreenSleep()
{
    if (screenSleep_ || !canScreenSleep()) return;
    screenSleep_ = true;
    sleepFrame_ = 0;
    sleepFrameMs_ = millis();
    M5Cardputer.Display.setBrightness(config_.screenSleepBrightness);
    dirty_ = true;
}

void App::exitScreenSleep()
{
    if (!screenSleep_) return;
    screenSleep_ = false;
    lastInputMs_ = millis();
    M5Cardputer.Display.setBrightness(awakeBrightness_);
    dirty_ = true;
}

void App::serviceScreenSleep()
{
    const std::uint32_t now = millis();
    if (screenSleep_) {
        if (!canScreenSleep()) {
            exitScreenSleep();
            return;
        }
        if (sleepMotion_ && now - sleepFrameMs_ >= kSleepFrameIntervalMs) {
            sleepFrameMs_ = now;
            ++sleepFrame_;
            dirty_ = true;
        }
        return;
    }
    if (canScreenSleep() &&
        now - lastInputMs_ >=
            static_cast<std::uint32_t>(config_.screenSleepSeconds) * 1000U) {
        enterScreenSleep();
    }
}

void App::submitCompose()
{
    compose_.trim();
    if (!compose_.length() || !activeSessionId_.length()) return;
    const String text = compose_;
    compose_ = "";
    if (composeMode_ == ComposeMode::kSteer) {
        JsonDocument params;
        params["session_id"] = activeSessionId_;
        params["text"] = text;
        steerRequestId_ =
            hermes_.request("session.steer", params.as<JsonObjectConst>());
        screen_ = Screen::kChat;
        status_ = "STEER QUEUED";
        return;
    }
    if (text.startsWith("/")) {
        startCommand(text);
        screen_ = Screen::kChat;
        status_ = "COMMAND SENT";
        return;
    }
    if (!submitText(text)) compose_ = text;
}

bool App::submitText(const String& text, const String& displayText)
{
    if (!text.length() || !activeSessionId_.length()) return false;
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    params["text"] = text;
    if (!hermes_.request("prompt.submit", params.as<JsonObjectConst>())) {
        status_ = "PROMPT SEND FAILED";
        return false;
    }
    currentAssistantText_ = "";
    appendTimeline("YOU: " + (displayText.length() ? displayText : text) +
                   "\n\nHERMES: ");
    if (pendingVoiceTranscript_ == text) pendingVoiceTranscript_ = "";
    screen_ = Screen::kChat;
    status_ = "PROMPT SENT";
    return true;
}

void App::startCommand(const String& command, bool alias)
{
    String normalized = command;
    normalized.trim();
    while (normalized.startsWith("/")) normalized.remove(0, 1);
    const int separator = normalized.indexOf(' ');
    pendingCommandName_ = separator < 0 ? normalized
                                        : normalized.substring(0, separator);
    pendingCommandArg_ = separator < 0 ? "" : normalized.substring(separator + 1);
    pendingCommandArg_.trim();
    pendingCommandDisplay_ = "/" + normalized;
    if (!alias) commandAliasDepth_ = 0;
    if (!pendingCommandName_.length()) {
        status_ = "EMPTY HERMES COMMAND";
        return;
    }
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    params["command"] = normalized;
    slashRequestId_ =
        hermes_.request("slash.exec", params.as<JsonObjectConst>());
}

void App::dispatchPendingCommand()
{
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    params["name"] = pendingCommandName_;
    params["arg"] = pendingCommandArg_;
    commandRequestId_ =
        hermes_.request("command.dispatch", params.as<JsonObjectConst>());
    status_ = "COMMAND FALLBACK";
}

void App::handleCommandResult(JsonVariantConst result)
{
    const String type = result["type"] | "";
    if (!type.length()) {
        const String warning = result["warning"] | "";
        const String output = result["output"] | "";
        if (warning.length()) appendTimeline("\n[WARNING] " + warning + "\n");
        appendTimeline("\n[COMMAND] " +
                       (output.length() ? output : pendingCommandDisplay_ +
                                                    ": no output") +
                       "\n");
        status_ = "COMMAND COMPLETE";
        return;
    }
    if (type == "exec" || type == "plugin") {
        appendTimeline("\n[COMMAND] " +
                       String(result["output"] | "(no output)") + "\n");
        status_ = "COMMAND COMPLETE";
    } else if (type == "alias") {
        const String target = result["target"] | "";
        if (!target.length() || ++commandAliasDepth_ > 4) {
            status_ = "COMMAND ALIAS LOOP";
            return;
        }
        startCommand("/" + target +
                         (pendingCommandArg_.length() ? " " + pendingCommandArg_
                                                     : ""),
                     true);
        status_ = "COMMAND ALIAS";
    } else if (type == "skill" || type == "send") {
        const String message = result["message"] | "";
        if (!message.length()) {
            status_ = "COMMAND MESSAGE MISSING";
            return;
        }
        const String notice = result["notice"] | "";
        if (notice.length()) appendTimeline("\n[COMMAND] " + notice + "\n");
        const String display = result["display"] | pendingCommandDisplay_;
        submitText(message, display);
    } else {
        status_ = "COMMAND RESPONSE INVALID";
    }
}

void App::speakLastResponse()
{
    if (!lastAssistantText_.length() || voice_.active()) {
        status_ = "NO HERMES RESPONSE TO SPEAK";
        return;
    }
    screen_ = Screen::kPlayback;
    status_ = "HERMES SYNTHESIZING SPEECH";
    dirty_ = true;
    draw();
    // A second TLS client cannot reliably allocate while the gateway
    // WebSocket still owns its TLS buffers. Release it before validating the
    // cookie and starting the long-running audio request.
    hermes_.disconnect();
    activeSessionId_ = "";
    delay(20);
    if (!hermes_.refreshAuthentication()) {
        screen_ = Screen::kChat;
        status_ = hermes_.diagnostic();
        dirty_ = true;
        return;
    }
    String error;
    const bool spoken = audioClient_.speak(lastAssistantText_, kTtsPath, error);
    screen_ = Screen::kChat;
    status_ = spoken ? "SPEECH COMPLETE - RECONNECTING" : error;
    dirty_ = true;
}

void App::startVoice()
{
    if (!activeSessionId_.length()) return;
    if (!voice_.begin(kVoicePath)) {
        status_ = voice_.error();
        return;
    }
    screen_ = Screen::kRecording;
    status_ = "LISTENING";
    recordingFrameMs_ = 0;
}

void App::startVoiceTest()
{
    if (!voice_.begin(kVoicePath)) {
        status_ = voice_.error();
        return;
    }
    voiceTest_ = true;
    screen_ = Screen::kRecording;
    status_ = "LISTENING";
    recordingFrameMs_ = 0;
}

void App::finishVoice(bool submit)
{
    if (!voice_.active()) return;
    if (voiceTest_) {
        bool complete = false;
        if (submit) {
            status_ = "FINALIZING MIC TEST";
            dirty_ = true;
            draw();
            complete = voice_.finish();
        } else {
            voice_.cancel();
        }
        SD.remove(kVoicePath);
        voiceTest_ = false;
        helpPage_ = 2;
        screen_ = Screen::kHelpSettings;
        status_ = submit ? (complete ? "MIC TEST COMPLETE" : voice_.error())
                         : "MIC TEST CANCELLED";
        return;
    }
    if (!submit) {
        voice_.cancel();
        screen_ = Screen::kChat;
        status_ = "VOICE CANCELLED";
        return;
    }
    status_ = "FINALIZING VOICE";
    dirty_ = true;
    draw();
    if (!voice_.finish()) {
        status_ = voice_.error();
        screen_ = Screen::kChat;
        return;
    }
    status_ = "HERMES TRANSCRIBING";
    dirty_ = true;
    draw();
    // Free the WebSocket TLS context before opening the ticket and upload TLS
    // clients. Keeping both alive after capture produces HTTPClient -1 on the
    // ESP32-S3 when the second TLS allocation fails.
    hermes_.disconnect();
    activeSessionId_ = "";
    delay(20);
    if (!hermes_.refreshAuthentication()) {
        status_ = hermes_.diagnostic();
        SD.remove(kVoicePath);
        screen_ = Screen::kChat;
        return;
    }
    String transcript;
    String error;
    const bool transcribed =
        audioClient_.transcribeWav(kVoicePath, transcript, error);
    SD.remove(kVoicePath);
    screen_ = Screen::kChat;
    if (!transcribed) {
        status_ = error;
        return;
    }
    // The TLS upload deliberately released the WebSocket. Keep the recognized
    // text visible, then submit it only after session.resume returns the new
    // runtime session id. Calling update() once here races that async response.
    pendingVoiceTranscript_ = transcript;
    compose_ = transcript;
    composeMode_ = ComposeMode::kPrompt;
    screen_ = Screen::kCompose;
    status_ = "TRANSCRIPT READY - SENDING";
}

void App::respondInteraction(const String& value)
{
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    params["request_id"] = interactionId_;
    const char* method = "clarify.respond";
    if (interactionType_ == "approval.request") {
        method = "approval.respond";
        params["choice"] = value;
    } else if (interactionType_ == "sudo.request") {
        method = "sudo.respond";
        params["password"] = value;
    } else if (interactionType_ == "secret.request") {
        method = "secret.respond";
        params["value"] = value;
    } else if (screen_ == Screen::kInteraction) {
        params["answer"] = value;
        if (!clarifyQuestions_.empty() &&
            clarifyQuestionIndex_ < clarifyQuestions_.size()) {
            params["question_id"] =
                clarifyQuestions_[clarifyQuestionIndex_].id;
        }
    }
    hermes_.request(method, params.as<JsonObjectConst>());
    compose_ = "";
    if (interactionType_ == "clarify.request" &&
        clarifyQuestionIndex_ + 1 < clarifyQuestions_.size()) {
        ++clarifyQuestionIndex_;
        interactionPrompt_ =
            "[" + String(clarifyQuestionIndex_ + 1) + "/" +
            String(clarifyQuestions_.size()) + "] " +
            clarifyQuestions_[clarifyQuestionIndex_].prompt;
        status_ = "NEXT HERMES QUESTION";
        return;
    }
    clarifyQuestions_.clear();
    clarifyQuestionIndex_ = 0;
    interactionId_ = "";
    screen_ = Screen::kChat;
    status_ = "RESPONSE SENT";
}

void App::prepareClarify(JsonObjectConst payload)
{
    clarifyQuestions_.clear();
    clarifyQuestionIndex_ = 0;
    JsonArrayConst questions = payload["questions"].as<JsonArrayConst>();
    JsonObjectConst answers = payload["answers"].as<JsonObjectConst>();
    for (JsonObjectConst question : questions) {
        const String id = question["qid"] | question["id"] | "";
        if (!id.length() || (!answers.isNull() && !answers[id].isNull())) continue;
        ClarifyQuestion item;
        item.id = id;
        item.prompt = question["question"] | "Hermes question";
        JsonArrayConst choices = question["choices"].as<JsonArrayConst>();
        if (!choices.isNull() && choices.size()) {
            item.prompt += "\nChoices: ";
            bool first = true;
            for (const char* choice : choices) {
                if (!first) item.prompt += ", ";
                item.prompt += choice;
                first = false;
            }
        }
        if (question["multi_select"] | false) {
            item.prompt += "\n(comma-separated selections accepted)";
        }
        clarifyQuestions_.push_back(item);
    }
    if (!clarifyQuestions_.empty()) {
        interactionPrompt_ = "[1/" + String(clarifyQuestions_.size()) + "] " +
                             clarifyQuestions_[0].prompt;
    } else {
        interactionPrompt_ = payload["question"] | "Pending Hermes question";
        JsonArrayConst choices = payload["choices"].as<JsonArrayConst>();
        if (!choices.isNull() && choices.size()) {
            interactionPrompt_ += "\nChoices: ";
            bool first = true;
            for (const char* choice : choices) {
                if (!first) interactionPrompt_ += ", ";
                interactionPrompt_ += choice;
                first = false;
            }
        }
    }
}

String App::shortText(const String& value, std::size_t maxLength) const
{
    return value.length() <= maxLength ? value : value.substring(0, maxLength - 1) + "~";
}

void App::draw()
{
    dirty_ = false;
    if (screenSleep_) {
        drawScreenSleep();
        return;
    }
    if (screen_ == Screen::kSessions) {
        drawSessionsScreen();
        return;
    }
    if (screen_ == Screen::kRecording) {
        drawRecordingScreen();
        return;
    }
    if (screen_ == Screen::kHelpSettings) {
        drawHelpSettingsScreen();
        return;
    }
    auto& display = M5Cardputer.Display;
    display.fillScreen(kUiBg);
    const char* section = screen_ == Screen::kChat ? "CHAT"
                          : screen_ == Screen::kCompose ? "TYPE"
                          : screen_ == Screen::kInteraction ? "INPUT"
                          : "AUDIO";
    drawPocketHeader(display, section, hermes_.connected());
    const char* activity = activityLabel(status_);
    const bool showStatus = strcmp(activity, "READY") != 0;
    display.setTextColor(kUiRed, kUiBg);
    display.setCursor(4, 18);
    display.print(shortText(screen_ == Screen::kChat &&
                                    activeSessionTitle_.length() && !showStatus
                                ? activeSessionTitle_ : status_, 24));
    if (usageText_.length()) {
        display.setTextColor(kUiMuted, kUiBg);
        display.setCursor(162, 18);
        display.print(shortText(usageText_, 5));
    }
    display.fillRoundRect(199, 17, 37, 9, 2, kUiRedDark);
    display.setTextColor(kUiInk, kUiRedDark);
    display.setCursor(203, 18);
    display.print(activity);
    display.setTextColor(kUiInk, kUiBg);
    drawBackgroundAccents(display);

    if (screen_ == Screen::kChat) {
        if (resumeRequestId_ && !activeSessionId_.length()) {
            display.fillRoundRect(5, 34, 230, 76, 3, kUiPanel);
            display.drawRoundRect(5, 34, 230, 76, 3, kUiRule);
            display.fillRect(5, 34, 4, 76, kUiRed);
            display.setTextColor(kUiRed, kUiPanel);
            display.setCursor(18, 45);
            display.print("LOADING SESSION");
            display.setTextColor(kUiInk, kUiPanel);
            display.setCursor(18, 62);
            display.print(shortText(activeSessionTitle_, 32));
            display.setTextColor(kUiMuted, kUiPanel);
            display.setCursor(18, 80);
            display.print("Restoring Hermes context...");
            for (int block = 0; block < 8; ++block) {
                display.drawRect(18 + block * 14, 96, 10, 5, kUiRule);
                if (block < 3) {
                    display.fillRect(19 + block * 14, 97, 8, 3, kUiRed);
                }
            }
            drawPocketFooter(display, "ESC CANCEL                 PLEASE WAIT");
            return;
        }
        display.setCursor(4, 34);
        display.setTextColor(kUiInk, kUiBg);
        const int charsPerLine = 39;
        std::vector<String> lines;
        String line;
        for (std::size_t i = 0; i < timeline_.length(); ++i) {
            const char c = timeline_[i];
            if (c == '\n' || line.length() >= charsPerLine) {
                lines.push_back(line); line = "";
                if (c == '\n') continue;
            }
            line += c;
        }
        if (line.length()) lines.push_back(line);
        const int visible = 10;
        const int start = scroll_ >= 32767 ? max(0, static_cast<int>(lines.size()) - visible)
                                           : min(scroll_, max(0, static_cast<int>(lines.size()) - visible));
        for (int row = 0; row < visible && start + row < static_cast<int>(lines.size()); ++row) {
            display.setCursor(4, 32 + row * 9);
            const String& shown = lines[start + row];
            if (shown.startsWith("YOU: ")) {
                display.setTextColor(kUiRed, kUiBg);
                display.print("YOU /");
                display.setTextColor(kUiInk, kUiBg);
                display.setCursor(34, 32 + row * 9);
                display.print(shown.substring(5));
            } else if (shown.startsWith("HERMES: ")) {
                display.setTextColor(kUiMuted, kUiBg);
                display.print("HERMES /");
                display.setTextColor(kUiInk, kUiBg);
                display.setCursor(52, 32 + row * 9);
                display.print(shown.substring(8));
            } else {
                display.setTextColor(shown.startsWith("[") ? kUiMuted : kUiInk,
                                     kUiBg);
                display.print(shown);
            }
        }
        if (static_cast<int>(lines.size()) > visible) {
            const int maximum = static_cast<int>(lines.size()) - visible;
            const int thumbHeight = max(7, 84 * visible /
                                           static_cast<int>(lines.size()));
            const int thumbY = 32 + (84 - thumbHeight) * start / maximum;
            display.drawFastVLine(239, 32, 84, kUiRule);
            display.fillRect(237, thumbY, 3, thumbHeight, kUiRed);
        }
        drawPocketFooter(display, "T TYPE  V VOICE  R READ  ` LIST");
    } else if (screen_ == Screen::kCompose) {
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(4, 34);
        display.print(composeMode_ == ComposeMode::kSteer ? "STEER" : "PROMPT / COMMAND");
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(4, 49);
        String visible = compose_.length() > 420 ? compose_.substring(compose_.length() - 420) : compose_;
        display.setTextWrap(true);
        display.print(visible);
        display.setTextWrap(false);
        drawPocketFooter(display, "ENTER SEND                 ` CANCEL");
    } else if (screen_ == Screen::kInteraction) {
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(4, 34);
        display.print(shortText(interactionType_, 36));
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(4, 49);
        display.setTextWrap(true);
        display.print(shortText(interactionPrompt_, 300));
        display.setTextWrap(false);
        drawPocketFooter(display, interactionType_ == "approval.request"
                                      ? "O ONCE S SESSION A ALWAYS D DENY"
                                      : "TYPE ANSWER ENTER SEND ` CANCEL");
    } else {
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(4, 40);
        display.setTextFont(2);
        display.print("HERMES SPEAKING");
        display.setTextFont(1);
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(4, 70);
        display.print("Synthesizing / playing audio...");
    }
}

void App::drawRecordingScreen()
{
    auto& display = M5Cardputer.Display;
    M5Canvas* canvas = fullScreenCanvas();
    if (!canvas) {
        display.fillScreen(kUiBg);
        display.setTextFont(2);
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(50, 48);
        display.print(shortText(status_, 16));
        return;
    }

    const unsigned long elapsed = voice_.elapsedMs();
    const unsigned long seconds = elapsed / 1000;
    const unsigned long tenths = (elapsed % 1000) / 100;
    const bool listening = voice_.active() && status_ == "LISTENING";

    canvas->fillScreen(kUiBg);
    canvas->setTextWrap(false);
    canvas->setTextFont(1);
    canvas->setTextColor(kUiInk, kUiBg);
    canvas->setCursor(4, 3);
    canvas->print("HERMES // VOICE");
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(199, 3);
    canvas->print("LINK");
    canvas->fillCircle(233, 6, 3,
                       hermes_.connected() ? TFT_GREEN : TFT_ORANGE);
    canvas->drawFastHLine(0, 14, display.width(), kUiRed);

    canvas->fillRoundRect(5, 22, 230, 88, 3, kUiPanel);
    canvas->drawRoundRect(5, 22, 230, 88, 3, listening ? kUiRed : kUiRule);
    if (listening) {
        canvas->fillCircle(27, 48, 7, kUiRed);
        canvas->drawCircle(27, 48, 10, kUiRedDark);
    } else {
        canvas->fillCircle(30, 48, 7, TFT_ORANGE);
    }

    canvas->setTextFont(2);
    canvas->setTextColor(listening ? kUiRed : kUiMuted, kUiPanel);
    canvas->setCursor(45, 37);
    canvas->print(listening ? (voiceTest_ ? "MIC TEST" : "RECORDING")
                            : shortText(status_, 18));

    canvas->setTextFont(1);
    canvas->setTextColor(kUiInk, kUiPanel);
    canvas->setCursor(45, 59);
    if (listening) {
        canvas->printf("%02lu:%02lu.%lu  /  00:%02lu",
                       seconds / 60,
                       seconds % 60,
                       tenths,
                       kMaxVoiceMs / 1000);
    } else {
        canvas->print("PLEASE WAIT");
    }

    canvas->setTextColor(kUiMuted, kUiPanel);
    canvas->setCursor(15, 76);
    canvas->print("MIC");
    const std::uint8_t level = listening ? voice_.levelBars() : 0;
    for (int bar = 0; bar < 12; ++bar) {
        const int x = 45 + bar * 15;
        canvas->drawRect(x, 76, 11, 7, kUiRule);
        if (bar < level) canvas->fillRect(x + 1, 77, 9, 5, kUiRed);
    }

    canvas->drawRoundRect(15, 91, 210, 8, 3, kUiRule);
    if (listening) {
        const unsigned long rawProgress = 206 * elapsed / kMaxVoiceMs;
        const int progress = static_cast<int>(rawProgress > 206 ? 206 : rawProgress);
        if (progress > 0) canvas->fillRect(17, 93, progress, 4, kUiRed);
    }
    canvas->setTextColor(kUiMuted, kUiPanel);
    canvas->setCursor(15, 102);
    canvas->print(listening ? "REC / MAX 30 SEC" : "PROCESSING AUDIO");

    canvas->drawFastHLine(0, 120, display.width(), kUiRule);
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(4, 125);
    canvas->print(voiceTest_ ? "ENTER DONE                  ESC CANCEL"
                             : "ENTER SEND                  ESC CANCEL");

    // Present one complete LCD frame; no visible clear/draw passes.
    canvas->pushSprite(0, 0);
}

void App::drawHelpSettingsScreen()
{
    auto& display = M5Cardputer.Display;
    display.fillScreen(kUiBg);
    drawPocketHeader(display, helpPage_ == 0 ? "MANUAL"
                              : helpPage_ == 1 ? "SETUP" : "STATUS",
                     hermes_.connected());
    display.setTextColor(kUiInk, kUiBg);
    display.setCursor(4, 20);
    drawBackgroundAccents(display);
    if (helpPage_ == 0) {
        auto drawHelpCard = [&](int y, const char* title) {
            display.fillRoundRect(3, y, 234, 20, 3, kUiPanel);
            display.fillRect(3, y + 2, 3, 16, kUiRed);
            display.drawFastVLine(55, y + 3, 14, kUiRule);
            display.setTextColor(kUiRed, kUiPanel);
            display.setCursor(9, y + 6);
            display.print(title);
        };
        auto drawKeyHint = [&](int x, int y, const char* key,
                               const char* action) {
            const int keyWidth = static_cast<int>(strlen(key)) * 6 + 6;
            display.fillRoundRect(x, y, keyWidth, 12, 2, kUiRedDark);
            display.setTextColor(kUiInk, kUiRedDark);
            display.setCursor(x + 3, y + 2);
            display.print(key);
            x += keyWidth + 3;
            display.setTextColor(kUiInk, kUiPanel);
            display.setCursor(x, y + 2);
            display.print(action);
            return x + static_cast<int>(strlen(action)) * 6 + 9;
        };

        drawHelpCard(20, "NAV");
        int x = drawKeyHint(61, 24, ";.", "MOVE");
        x = drawKeyHint(x, 24, "ENT", "OPEN");
        drawKeyHint(x, 24, "R", "SYNC");

        drawHelpCard(43, "CREATE");
        x = drawKeyHint(61, 47, "N", "TEXT");
        drawKeyHint(x, 47, "V", "VOICE");

        drawHelpCard(66, "CHAT");
        x = drawKeyHint(61, 70, "T", "TYPE");
        x = drawKeyHint(x, 70, "V", "VOICE");
        drawKeyHint(x, 70, "R", "READ");

        drawHelpCard(89, "CONTROL");
        x = drawKeyHint(61, 93, "S", "STEER");
        x = drawKeyHint(x, 93, "X", "STOP");
        drawKeyHint(x, 93, "Z", "SLEEP");
    } else if (helpPage_ == 1) {
        display.setTextColor(kUiMuted, kUiBg);
        display.printf("PROFILE %s / %s",
                       shortText(config_.profile.length() ? config_.profile
                                                         : String("default"), 10).c_str(),
                       shortText(hermes_.authMode(), 10).c_str());
        const char* labels[] = {"DISPLAY / AWAKE", "DISPLAY / SLEEP",
                                "DISPLAY / DIM", "AUDIO / TTS",
                                "SLEEP / MOTION", "AUDIO / ALERTS"};
        const int firstSetting = settingRow_ >= 5 ? 1 : 0;
        for (int row = 0; row < 5; ++row) {
            const int setting = firstSetting + row;
            const int y = 31 + row * 17;
            const bool selected = setting == settingRow_;
            const std::uint16_t background = selected ? kUiRedDark : kUiBg;
            if (selected) {
                display.fillRoundRect(2, y - 2, 234, 15, 2, background);
                display.fillRect(2, y - 2, 3, 15, kUiRed);
            }
            display.setTextColor(selected ? kUiInk : kUiMuted, background);
            display.setCursor(8, y);
            display.print(labels[setting]);
            display.setTextColor(kUiInk, background);
            display.setCursor(181, y);
            if (setting == 0) display.printf("%3u", awakeBrightness_);
            else if (setting == 1) display.printf("%3us", config_.screenSleepSeconds);
            else if (setting == 2) display.printf("%3u", config_.screenSleepBrightness);
            else if (setting == 3) display.printf("%3u", config_.ttsVolume);
            else if (setting == 4) display.print(sleepMotion_ == 0 ? "OFF"
                                            : sleepMotion_ == 1 ? "LOW" : "HIGH");
            else display.print(alertsEnabled_ ? " ON" : "OFF");
            if (selected) {
                display.setTextColor(kUiRed, background);
                display.setCursor(169, y);
                display.print('<');
                display.setCursor(205, y);
                display.print('>');
            }
        }
    } else {
        display.printf(
            "DEVICE   HM-01 / FW 2026.08.22\n\n"
            "WIFI     %s\n"
            "IP       %s\n"
            "SIGNAL   %d DBM\n"
            "GATEWAY  %s\n"
            "HERMES   %s\n"
            "THEME    POCKET TERMINAL 01\n\n"
            "STATE    %s",
            shortText(WiFi.SSID(), 25).c_str(),
            WiFi.localIP().toString().c_str(), WiFi.RSSI(),
            shortText(config_.baseUrl, 25).c_str(),
            hermes_.connected() ? "LINKED" : "OFFLINE",
            shortText(status_, 25).c_str());
    }
    drawPocketFooter(display, helpPage_ == 0
                                  ? "S SETUP  D STATUS  ESC / GO BACK"
                              : helpPage_ == 1
                                  ? "^v ITEM  <> CHANGE  D STATUS  ESC SAVE"
                                  : "M MIC  K SPEAKER  R RECONNECT  H HELP");
}

void App::drawSessionsScreen()
{
    M5Canvas* canvas = fullScreenCanvas();
    if (!canvas) {
        auto& lcd = M5Cardputer.Display;
        lcd.fillScreen(kUiBg);
        drawPocketHeader(lcd, "SESSIONS", hermes_.connected());
        lcd.setTextColor(kUiInk, kUiBg);
        lcd.setCursor(8, 40);
        lcd.print(shortText(status_, 36));
        return;
    }
    auto& display = *canvas;
    constexpr int kVisibleRows = 5;
    constexpr int kListTop = 30;
    constexpr int kRowHeight = 18;

    display.fillScreen(kUiBg);
    display.setTextFont(1);
    display.setTextWrap(false);

    drawPocketHeader(display, "SESSIONS", hermes_.connected());
    drawBackgroundAccents(display);

    const char* footer = "^v MOVE  ENTER OPEN  N NEW  V VOICE";
    if (sessions_.empty()) {
        const bool loadingSessions = hermes_.connected() && sessionsRequestId_;
        const bool localError = status_.indexOf("ERROR") >= 0 ||
                                status_.indexOf("FAILED") >= 0 ||
                                status_.indexOf("INVALID") >= 0 ||
                                status_.indexOf("REQUIRED") >= 0;
        const bool connectionError = hermes_.connectionFailed() || localError;
        if (!hermes_.connected() || loadingSessions || connectionError) {
            display.fillRoundRect(5, 35, 230, 70, 3, kUiPanel);
            display.drawRoundRect(5, 35, 230, 70, 3,
                                  connectionError ? kUiRed : kUiRule);
            display.fillRect(5, 35, 4, 70, kUiRed);
            display.setTextColor(kUiRed, kUiPanel);
            display.setCursor(18, 46);
            display.print(connectionError ? "CONNECTION ERROR"
                          : loadingSessions ? "LOADING SESSIONS"
                                            : "CONNECTING TO HERMES");
            display.setTextColor(connectionError ? kUiInk : kUiMuted,
                                 kUiPanel);
            display.setCursor(18, 64);
            display.print(shortText(connectionError
                                        ? (hermes_.connectionFailed()
                                               ? hermes_.diagnostic() : status_)
                                        : config_.baseUrl, 34));
            display.setTextColor(kUiMuted, kUiPanel);
            display.setCursor(18, 82);
            display.print(connectionError ? "Automatic retry is active"
                          : loadingSessions ? "Waiting for session list..."
                                            : "Authorizing secure link...");
            if (!connectionError) {
                const int lit = 1 + (millis() / 180) % 8;
                for (int block = 0; block < 8; ++block) {
                    display.drawRect(18 + block * 14, 95, 10, 5, kUiRule);
                    if (block < lit) {
                        display.fillRect(19 + block * 14, 96, 8, 3, kUiRed);
                    }
                }
            }
            footer = connectionError ? "R RETRY  D DETAILS  HOLD GO HELP"
                     : "PLEASE WAIT              D DETAILS";
        } else {
            display.setTextColor(kUiMuted, kUiBg);
            display.setCursor(4, 19);
            display.print("NO SAVED SESSIONS");
            display.setTextColor(kUiRed, kUiBg);
            display.setCursor(61, 61);
            display.print("PRESS N TO CREATE");
        }
    } else {
        const int count = static_cast<int>(sessions_.size());
        const int selected = min(max(selectedSession_, 0), count - 1);
        const int maxStart = max(0, count - kVisibleRows);
        const int windowStart = min(max(selected - kVisibleRows / 2, 0), maxStart);
        const int windowEnd = min(count, windowStart + kVisibleRows);

        display.setTextColor(kUiMuted, kUiBg);
        display.setCursor(4, 18);
        display.printf("SELECT A SESSION / %02d", count);
        display.setCursor(205, 18);
        display.printf("%02d/%02d", selected + 1, count);

        for (int index = windowStart; index < windowEnd; ++index) {
            const int row = index - windowStart;
            const int y = kListTop + row * kRowHeight;
            const bool isSelected = index == selected;
            const std::uint16_t background = isSelected ? kUiRedDark : kUiBg;

            if (isSelected) {
                display.fillRoundRect(2, y, 234, kRowHeight - 1, 2, background);
                display.fillRect(2, y, 3, kRowHeight - 1, kUiRed);
            } else {
                display.drawFastHLine(22, y + kRowHeight - 1, 212, kUiRule);
            }

            display.setTextColor(isSelected ? kUiInk : kUiRed, background);
            display.setCursor(6, y + 1);
            display.printf("%02d", index + 1);

            const String title = singleLine(sessions_[index].title);
            display.setTextColor(kUiInk, background);
            display.setCursor(23, y + 1);
            display.print(shortText(title.length() ? title : String("Untitled"), 35));

            String metadata = singleLine(sessions_[index].preview);
            String sessionState = singleLine(sessions_[index].state);
            sessionState.toUpperCase();
            if (sessionState.length()) {
                metadata = shortText(sessionState, 8) + " / " + metadata;
            }
            if (!metadata.length() || metadata == title) {
                const String& id = sessions_[index].id;
                metadata = "SAVED  ";
                metadata += id.length() > 8 ? id.substring(id.length() - 8) : id;
            }
            display.setTextColor(isSelected ? kUiInk : kUiMuted, background);
            display.setCursor(23, y + 9);
            display.print(shortText(metadata, 35));
        }

        if (count > kVisibleRows) {
            constexpr int kTrackTop = kListTop;
            constexpr int kTrackHeight = kVisibleRows * kRowHeight - 1;
            const int thumbHeight = max(8, kTrackHeight * kVisibleRows / count);
            const int thumbTravel = kTrackHeight - thumbHeight;
            const int thumbY = kTrackTop +
                               (maxStart ? thumbTravel * windowStart / maxStart : 0);
            display.drawFastVLine(239, kTrackTop, kTrackHeight, kUiRule);
            display.fillRect(237, thumbY, 3, thumbHeight, kUiRed);
        }
    }

    drawPocketFooter(display, footer);
    // The animated connection rail is composed off-screen. The LCD only sees
    // completed frames, so its periodic update cannot flash a cleared screen.
    canvas->pushSprite(0, 0);
}

void App::drawScreenSleep()
{
    auto& display = M5Cardputer.Display;
    M5Canvas* canvas = fullScreenCanvas();
    if (!canvas) {
        display.fillScreen(kUiBg);
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(8, 8);
        display.print(hermes_.connected() ? "HERMES ONLINE" : "HERMES OFFLINE");
        display.fillCircle(228, 10, 3,
                           hermes_.connected() ? TFT_GREEN : TFT_ORANGE);
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(8, 28);
        display.print(shortText(status_, 36));
        return;
    }

    canvas->fillScreen(kUiBg);
    canvas->drawFastVLine(99, 17, 101, kUiRule);
    canvas->fillRect(97, 24, 5, 2, kUiRed);
    canvas->fillRect(97, 104, 5, 2, kUiRed);
    const int phase = (sleepFrame_ / 10) % 4;
    const int rawBob = phase <= 2 ? phase : 4 - phase;
    const int bob = sleepMotion_ == 2 ? rawBob
                    : sleepMotion_ == 1 ? (rawBob + 1) / 2 : 0;
    const char* sleepActivity = activityLabel(status_);
    const bool workingPortrait = strcmp(sleepActivity, "WORK") == 0 ||
                                 strcmp(sleepActivity, "TOOL") == 0;
    const bool blinkPortrait = sleepMotion_ && !workingPortrait &&
                               strcmp(sleepActivity, "READY") == 0 &&
                               sleepFrame_ % 80 >= 72;
    const std::uint8_t* portrait = kHermesBadge2Bpp;
    if (workingPortrait) {
        portrait = kHermesWorking2Bpp;
    } else if (blinkPortrait) {
        portrait = kHermesBlink2Bpp;
    }
    // READY and blink share a fixed origin. Only the WORK/TOOL expression may
    // use the subtle configured motion, so blinking cannot shift the portrait.
    drawHermesBadge(*canvas, portrait, 5, 20 + (workingPortrait ? bob : 0));

    canvas->setTextFont(2);
    canvas->setTextColor(kUiInk, kUiBg);
    canvas->setCursor(106, 14);
    canvas->print("HERMES");
    canvas->setTextFont(1);
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(107, 36);
    canvas->print(hermes_.connected() ? "LINK / ONLINE" : "LINK / OFFLINE");
    canvas->fillCircle(226, 39, 3,
                       hermes_.connected() ? TFT_GREEN : TFT_ORANGE);

    canvas->setTextColor(kUiInk, kUiBg);
    const String compactStatus = shortText(status_, 42);
    canvas->setCursor(107, 55);
    canvas->print(compactStatus.substring(0, 21));
    if (compactStatus.length() > 21) {
        canvas->setCursor(107, 66);
        canvas->print(compactStatus.substring(21));
    }
    canvas->fillRoundRect(106, 83, 45, 11, 2, kUiRedDark);
    canvas->setTextColor(kUiInk, kUiRedDark);
    canvas->setCursor(111, 85);
    canvas->print(activityLabel(status_));
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(158, 85);
    canvas->print(activeSessionTitle_.length()
                      ? shortText(activeSessionTitle_, 12)
                      : "SESSION LIST");
    canvas->drawFastHLine(104, 103, 129, kUiRed);
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(107, 112);
    canvas->print("ANY KEY TO WAKE");

    // Push the complete frame at once; the physical LCD never sees a clear pass.
    canvas->pushSprite(0, 0);
}

}  // namespace hermes_terminal
