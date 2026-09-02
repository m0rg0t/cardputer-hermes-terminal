#include "hermes_terminal/app.h"

#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#ifndef HERMES_MDNS
#define HERMES_MDNS 0
#endif
#if HERMES_MDNS
#include <ESPmDNS.h>
#endif

#include "hermes_terminal/cache_rules.h"
#include "hermes_terminal/stream_text_rules.h"
#include "hermes_terminal/ui_rules.h"
#include "app_ui.h"

namespace hermes_terminal {
namespace {

constexpr std::uint8_t kSdClockPin = 40;
constexpr std::uint8_t kSdMisoPin = 39;
constexpr std::uint8_t kSdMosiPin = 14;
constexpr std::uint8_t kSdChipSelectPin = 12;
constexpr std::uint32_t kSdFrequencyHz = 10000000;
constexpr std::size_t kMaxTimeline = kCacheRamWindowBytes;
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
constexpr unsigned long kSessionsRefreshDebounceMs = 30000;

String jsonText(JsonVariantConst value)
{
    if (value.is<const char*>()) return String(value.as<const char*>());
    String text;
    serializeJson(value, text);
    return text;
}

String queryEncode(const String& value)
{
    static const char hex[] = "0123456789ABCDEF";
    String result;
    result.reserve(value.length() * 3);
    for (std::size_t index = 0; index < value.length(); ++index) {
        const std::uint8_t c = static_cast<std::uint8_t>(value[index]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') result += static_cast<char>(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0x0f];
        }
    }
    return result;
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
    cacheReady_ = cache_.begin(SD, config_);
    loadUiSettings();
    cache_.setEnabled(cacheEnabled_ && cacheReady_);
    cache_.setQuotaMb(cacheQuotaMb_);
    if (cache_.enabled() && cache_.loadSessions(sessions_, 12, 0)) {
        sessionsTotal_ = cache_.sessionCount();
        status_ = String(sessions_.size()) + " CACHED SESSIONS";
    }
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
    audioClient_.setUiCuesEnabled(alertsEnabled_);
    String cueError;
    (void)audioClient_.playUiCue(UiCue::kStartup, cueError);
    status_ = "CONNECTING";
#if HERMES_WEB_ADMIN
    if (!webAdmin_.begin(config_, *this)) {
        status_ = "WEB ADMIN CONFIG INVALID";
    }
#endif
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
        else if (key == "cache") cacheEnabled_ = value != "0";
        else if (key == "cache_mb") {
            const int requested = value.toInt();
            cacheQuotaMb_ = requested <= 8 ? 8 : requested <= 32 ? 32 : 128;
        }
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
    file.printf("awake=%u\nsleep=%u\ndim=%u\ntts=%u\nmotion=%u\nalerts=%u\ncache=%u\ncache_mb=%u\n",
                awakeBrightness_, config_.screenSleepSeconds,
                config_.screenSleepBrightness, config_.ttsVolume,
                sleepMotion_, alertsEnabled_ ? 1 : 0,
                cacheEnabled_ ? 1 : 0, cacheQuotaMb_);
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
            audioClient_.setUiCuesEnabled(alertsEnabled_);
            if (alertsEnabled_) {
                String cueError;
                (void)audioClient_.playUiCue(UiCue::kConnected, cueError);
            }
            break;
        case 6:
            cacheEnabled_ = !cacheEnabled_;
            cache_.setEnabled(cacheEnabled_ && cacheReady_);
            break;
        case 7:
            if (delta > 0)
                cacheQuotaMb_ = cacheQuotaMb_ == 8 ? 32 :
                                cacheQuotaMb_ == 32 ? 128 : 8;
            else
                cacheQuotaMb_ = cacheQuotaMb_ == 128 ? 32 :
                                cacheQuotaMb_ == 32 ? 8 : 128;
            cache_.setQuotaMb(cacheQuotaMb_);
            cache_.enforceQuota();
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
    serviceHistorySync();
    cache_.update();
#if HERMES_WEB_ADMIN
    webAdmin_.update();
#endif
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
            audioError_ = status_;
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
    if (audioError_.length() && status_ != audioError_) {
        status_ = audioError_;
        dirty_ = true;
    }
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
#if HERMES_MDNS
    const bool shouldRun = WiFi.status() == WL_CONNECTED
#if HERMES_WEB_ADMIN
                           && webAdmin_.enabled()
#else
                           && false
#endif
                           ;
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
#endif
}

void App::onHermesConnected()
{
    lastHermesDiagnostic_ = hermes_.diagnostic();
    if (!activeStoredSessionId_.length()) {
        String cueError;
        (void)audioClient_.playUiCue(UiCue::kConnected, cueError);
    }
    status_ = "HERMES CONNECTED";
    if (activeStoredSessionId_.length() && screen_ != Screen::kSessions) {
        JsonDocument params;
        params["session_id"] = activeStoredSessionId_;
        params["omit_messages"] = true;
        params["defer_history"] = true;
        if (config_.profile.length()) params["profile"] = config_.profile;
        resumeRequestId_ =
            hermes_.request("session.resume", params.as<JsonObjectConst>());
        status_ = "RESTORING SESSION";
    } else {
        if (sessionsFreshAfterRest_) {
            sessionsFreshAfterRest_ = false;
            status_ = String(sessionsTotal_) + " HERMES SESSIONS";
        } else {
            requestSessions();
        }
    }
    dirty_ = true;
}

void App::onHermesDisconnected(const String& reason)
{
    lastHermesDiagnostic_ = reason;
    activeSessionId_ = "";
    sessionsRequestId_ = 0;
    const bool createUncertain = createRequestId_ != 0;
    createRequestId_ = 0;
    pendingVoiceSession_ = false;
    const bool promptUncertain = promptRequestId_ != 0;
    if (promptUncertain) {
        cache_.updateMessageState(activeStoredSessionId_,
                                  String(promptRequestId_), "failed");
        promptRequestId_ = 0;
        const bool voicePrompt = pendingVoiceTranscript_.length();
        const String retryText = voicePrompt
                                     ? pendingVoiceTranscript_
                                     : pendingPromptText_;
        pendingPromptText_ = "";
        pendingVoiceTranscript_ = "";
        if (retryText.length()) {
            compose_ = retryText;
            composeMode_ = ComposeMode::kPrompt;
            screen_ = Screen::kCompose;
        }
        status_ = voicePrompt
                      ? "VOICE SEND UNCERTAIN - ENTER"
                      : "PROMPT SEND UNCERTAIN - ENTER";
    } else if (createUncertain) {
        status_ = "SESSION CREATE UNCERTAIN - REFRESH";
    } else {
        status_ = timelineFromCache_ ? "OFFLINE CACHE"
                                     : reason + " - RETRYING";
    }
    // A resumed session emits a new message.start if Hermes is still working.
    // Do not leave local compose/audio guards wedged after transport loss.
    turnInProgress_ = false;
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

#if HERMES_WEB_ADMIN
void App::writeWebStatus(JsonObject output)
{
    output["device"] = config_.hostname;
    output["ip"] = WiFi.localIP().toString();
    output["hermes_connected"] = hermes_.connected();
    output["status"] = status_;
    output["diagnostic"] = hermes_.diagnostic();
    output["auth_mode"] = hermes_.authMode();
    output["auth_configured"] = hermes_.authConfigured();
    output["session_title"] = activeSessionTitle_;
    output["recording"] = voice_.active();
    output["free_heap"] = ESP.getFreeHeap();
#ifndef HERMES_WEB_COMPACT_STATUS
#define HERMES_WEB_COMPACT_STATUS 0
#endif
#if !HERMES_WEB_COMPACT_STATUS
    output["gateway_auth_mode"] = hermes_.gatewayAuthMode();
    output["gateway_auth_required"] = hermes_.gatewayAuthRequired();
    output["gateway_auth_flows"] = hermes_.gatewayAuthFlows();
    output["session_id"] = activeSessionId_;
    output["stored_session_id"] = activeStoredSessionId_;
    output["mdns_name"] = mdnsName_;
    output["mdns_url"] = mdnsStarted_ ? String("http://") + mdnsName_ + ".local/" : "";
    output["mdns_http_service"] = mdnsStarted_;
#endif
}

bool App::updateWebAuthCookie(const String& cookie)
{
    String value = cookie;
    value.trim();
    // Keep this endpoint deliberately narrow: it accepts a Cookie header,
    // never a password or an arbitrary header blob. Reject control characters
    // before the value reaches the Hermes client or the SD-card runtime file.
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
#endif

void App::requestSessions()
{
    if (sessionsSyncPending_ || sessionsRequestId_ || createRequestId_) return;
    if (cache_.enabled() && hermes_.connected()) {
        const bool importReady = cache_.beginSessionsImport();
        if (importReady && startSessionsPage(0)) {
            status_ = sessions_.empty() ? "LOADING SESSIONS" : "SYNCING SESSIONS";
            return;
        }
        if (importReady) cache_.abortSessionsImport();
    }
    JsonDocument params;
    params["limit"] = 12;
    if (config_.profile.length()) params["profile"] = config_.profile;
    sessionsRequestId_ = hermes_.request("session.list", params.as<JsonObjectConst>());
    status_ = sessionsRequestId_ ? "LOADING SESSIONS" : "SESSION LIST FAILED";
}

bool App::startSessionsPage(std::size_t offset)
{
    String path = "/api/sessions?limit=100&offset=" + String(offset) +
                  "&order=recent";
    if (config_.profile.length()) path += "&profile=" + queryEncode(config_.profile);
    sessionsSyncPending_ = hermes_.beginRestDownload(
        path, SD, cache_.restStagingPath());
    if (sessionsSyncPending_) sessionsSyncOffset_ = offset;
    return sessionsSyncPending_;
}

void App::createSession(bool voiceFirst)
{
    pendingPromptText_ = "";
    pendingVoiceTranscript_ = "";
    voiceRetryAvailable_ = false;
    retryVoiceAfterResume_ = false;
    SD.remove(kVoicePath);
    turnInProgress_ = false;
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
    pendingPromptText_ = "";
    pendingVoiceTranscript_ = "";
    voiceRetryAvailable_ = false;
    retryVoiceAfterResume_ = false;
    SD.remove(kVoicePath);
    turnInProgress_ = false;
    activeStoredSessionId_ = sessions_[selectedSession_].id;
    cache_.protectSession(activeStoredSessionId_);
    activeSessionId_ = "";
    activeSessionTitle_ = sessions_[selectedSession_].title;
    timeline_ = "";
    lastAssistantText_ = "";
    timelineFromCache_ = cache_.loadTimelineWindow(
        activeStoredSessionId_, static_cast<std::size_t>(-1), timeline_,
        lastAssistantText_, cacheWindowStart_, cacheWindowEnd_,
        cacheTotalBytes_);
    scroll_ = timelineFromCache_ ? kScrollFollowBottom : 0;
    screen_ = Screen::kChat;
    historySyncAttempted_ = false;
    if (beginHistorySync()) {
        status_ = timelineFromCache_ ? "CACHED / SYNCING" : "LOADING HISTORY";
        return;
    }
    JsonDocument params;
    params["session_id"] = activeStoredSessionId_;
    params["omit_messages"] = true;
    params["defer_history"] = true;
    if (config_.profile.length()) params["profile"] = config_.profile;
    resumeRequestId_ =
        hermes_.request("session.resume", params.as<JsonObjectConst>());
    if (resumeRequestId_) {
        status_ = timelineFromCache_ ? "CACHED / SYNCING" : "RESUMING SESSION";
    } else if (timelineFromCache_) {
        // Offline, or the link is mid-reconnect after a REST sync: keep the
        // cached transcript readable. onHermesConnected resumes it later.
        status_ = "OFFLINE CACHE";
    } else {
        screen_ = Screen::kSessions;
        activeStoredSessionId_ = "";
        activeSessionTitle_ = "";
        status_ = sessionsSyncPending_ ? "WAIT FOR SESSION SYNC"
                                       : "SESSION LOAD FAILED";
    }
}

bool App::beginHistorySync()
{
    if (!cache_.enabled() || !activeStoredSessionId_.length() ||
        !hermes_.connected()) return false;
    if (timelineFromCache_) {
        if (!cache_.beginHistoryImport(activeStoredSessionId_)) return false;
        return startHistoryPage(HistorySyncPhase::kOldest, 0);
    }
    return startHistoryPage(HistorySyncPhase::kLatest, 0);
}

bool App::startHistoryPage(HistorySyncPhase phase, std::size_t offset)
{
    String path = "/api/sessions/" + queryEncode(activeStoredSessionId_) +
                  "/messages?limit=48&offset=" + String(offset) +
                  "&order=" +
                  (phase == HistorySyncPhase::kLatest ? "latest" : "oldest");
    if (config_.profile.length()) {
        path += "&profile=" + queryEncode(config_.profile);
    }
    historySyncAttempted_ = true;
    historySyncPending_ = hermes_.beginRestDownload(path, SD,
                                                     cache_.restStagingPath());
    if (historySyncPending_) {
        historySyncPhase_ = phase;
        historySyncOffset_ = offset;
    }
    return historySyncPending_;
}

void App::serviceHistorySync()
{
    HermesClient::RestDownloadResult result;
    if (!hermes_.takeRestDownloadResult(result)) return;
    if (sessionsSyncPending_) {
        sessionsSyncPending_ = false;
        sessionsFreshAfterRest_ = false;
        std::size_t records = 0;
        if (!result.cancelled && !result.error.length() &&
            result.status == 200 &&
            cache_.appendSessionsImportPage(result.path, records)) {
            if (pagedImportContinues(records, 100, sessionsSyncOffset_,
                                     kMaxSessionsImport) &&
                startSessionsPage(sessionsSyncOffset_ + records)) {
                status_ = "SYNCED " + String(sessionsSyncOffset_ + records) +
                          " SESSIONS";
                dirty_ = true;
                return;
            }
            if (cache_.commitSessionsImport()) {
                sessionsWindowOffset_ = 0;
                cache_.loadSessions(sessions_, 12, 0);
                sessionsTotal_ = cache_.sessionCount();
                selectedSession_ = 0;
                sessionsFreshAfterRest_ = true;
                lastSessionsSyncMs_ = millis();
                status_ = String(sessionsTotal_) + " SESSIONS CACHED";
            } else {
                cache_.abortSessionsImport();
                status_ = "SESSION SYNC INCOMPLETE";
            }
        } else {
            cache_.abortSessionsImport();
            if (result.path.length()) SD.remove(result.path);
            // A cancelled sync must not restart itself on the reconnect.
            if (result.cancelled) sessionsFreshAfterRest_ = true;
            status_ = result.cancelled ? "SESSION SYNC CANCELLED"
                      : result.error.length() ? result.error
                                              : "SESSIONS HTTP " +
                                                    String(result.status);
        }
        dirty_ = true;
        return;
    }
    historySyncPending_ = false;
    if (result.cancelled) {
        cache_.abortHistoryImport();
        historySyncPhase_ = HistorySyncPhase::kNone;
        status_ = "SESSION LOAD CANCELLED";
        dirty_ = true;
        return;
    }
    if (!result.error.length() && result.status == 200 &&
        historySyncPhase_ == HistorySyncPhase::kLatest &&
        cache_.replaceHistoryFromRest(activeStoredSessionId_, result.path)) {
        timeline_ = "";
        lastAssistantText_ = "";
        timelineFromCache_ = cache_.loadTimelineWindow(
            activeStoredSessionId_, static_cast<std::size_t>(-1), timeline_,
            lastAssistantText_, cacheWindowStart_, cacheWindowEnd_,
            cacheTotalBytes_);
        scroll_ = timelineFromCache_ ? kScrollFollowBottom : 0;
        status_ = "LATEST READY / SYNCING";
        if (cache_.beginHistoryImport(activeStoredSessionId_) &&
            startHistoryPage(HistorySyncPhase::kOldest, 0)) {
            dirty_ = true;
            return;
        }
        cache_.abortHistoryImport();
    } else if (!result.error.length() && result.status == 200 &&
               historySyncPhase_ == HistorySyncPhase::kOldest) {
        std::size_t records = 0;
        if (cache_.appendHistoryImportPage(result.path, records)) {
            if (pagedImportContinues(records, 48, historySyncOffset_,
                                     kMaxHistoryImport) &&
                startHistoryPage(HistorySyncPhase::kOldest,
                                 historySyncOffset_ + records)) {
                status_ = "SYNCED " + String(historySyncOffset_ + records) +
                          " MESSAGES";
                dirty_ = true;
                return;
            }
            if (cache_.commitHistoryImport()) {
                timeline_ = "";
                lastAssistantText_ = "";
                timelineFromCache_ = cache_.loadTimelineWindow(
                    activeStoredSessionId_, static_cast<std::size_t>(-1),
                    timeline_, lastAssistantText_, cacheWindowStart_,
                    cacheWindowEnd_, cacheTotalBytes_);
                scroll_ = timelineFromCache_ ? kScrollFollowBottom : 0;
                status_ = "HISTORY CACHED / CONNECTING";
            } else {
                cache_.abortHistoryImport();
                status_ = "HISTORY SYNC INCOMPLETE";
            }
        } else {
            cache_.abortHistoryImport();
            status_ = cache_.error();
        }
    } else {
        cache_.abortHistoryImport();
        if (result.path.length()) SD.remove(result.path);
        const String reason = result.error.length() ? result.error
                              : result.status == 200 && cache_.error().length()
                                  ? cache_.error()
                                  : "HISTORY HTTP " + String(result.status);
        status_ = timelineFromCache_ ? "CACHE LIVE / " + reason : reason;
        // Let session.resume fall back to session.history over the socket.
        historySyncAttempted_ = false;
    }
    historySyncPhase_ = HistorySyncPhase::kNone;
    dirty_ = true;
}

void App::returnToSessions()
{
    if (promptRequestId_) {
        cache_.updateMessageState(activeStoredSessionId_,
                                  String(promptRequestId_), "failed");
        promptRequestId_ = 0;
    }
    const bool cancelledRest = hermes_.restDownloadActive();
    if (hermes_.restDownloadActive()) {
        hermes_.cancelRestDownload();
        historySyncPending_ = false;
    }
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
    cache_.protectSession("");
    activeSessionTitle_ = "";
    currentAssistantText_ = "";
    lastInterimText_ = "";
    pendingPromptText_ = "";
    pendingVoiceTranscript_ = "";
    voiceRetryAvailable_ = false;
    retryVoiceAfterResume_ = false;
    SD.remove(kVoicePath);
    reasoningOpen_ = false;
    turnInProgress_ = false;
    historyRequestId_ = 0;
    historySyncAttempted_ = false;
    historySyncPhase_ = HistorySyncPhase::kNone;
    cache_.abortHistoryImport();
    cache_.abandonAssistantSpool();
    screen_ = Screen::kSessions;
    if (cancelledLoad || cancelledRest) status_ = "SESSION LOAD CANCELLED";
    if (!cancelledRest) requestSessions();
}

void App::requestHistory()
{
    if (!activeSessionId_.length()) return;
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    historyRequestId_ = hermes_.request("session.history", params.as<JsonObjectConst>());
}

void App::parseResponse(JsonObjectConst root)
{
    const std::uint32_t id = root["id"] | 0U;
    if (id && id == cancelledResumeRequestId_) {
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
        bool resumeVoiceRecovery = false;
        if (id == slashRequestId_) {
            dispatchPendingCommand();
            return;
        }
        if (id == createRequestId_) {
            pendingVoiceSession_ = false;
            createRequestId_ = 0;
        }
        if (id == promptRequestId_) {
            cache_.updateMessageState(activeStoredSessionId_, String(id),
                                      "failed");
            promptRequestId_ = 0;
            turnInProgress_ = false;
            const String retryText = pendingVoiceTranscript_.length()
                                         ? pendingVoiceTranscript_
                                         : pendingPromptText_;
            pendingPromptText_ = "";
            pendingVoiceTranscript_ = "";
            if (retryText.length()) {
                compose_ = retryText;
                composeMode_ = ComposeMode::kPrompt;
                screen_ = Screen::kCompose;
            }
        }
        if (id == sessionsRequestId_) sessionsRequestId_ = 0;
        if (id == resumeRequestId_) {
            resumeRequestId_ = 0;
            resumeVoiceRecovery = pendingVoiceTranscript_.length() ||
                                  voiceRetryAvailable_;
            retryVoiceAfterResume_ = false;
            if (pendingVoiceTranscript_.length()) {
                compose_ = pendingVoiceTranscript_;
                composeMode_ = ComposeMode::kPrompt;
                screen_ = Screen::kCompose;
            } else if (voiceRetryAvailable_) {
                screen_ = Screen::kChat;
            } else {
                activeStoredSessionId_ = "";
                activeSessionTitle_ = "";
                screen_ = Screen::kSessions;
            }
        }
        if (resumeVoiceRecovery) {
            status_ = pendingVoiceTranscript_.length()
                          ? "RESTORE FAILED - ENTER RETRY"
                          : "RESTORE FAILED - V RETRY";
        } else {
            status_ = "RPC ERROR " + jsonText(root["error"]);
        }
        return;
    }
    JsonVariantConst result = root["result"];
    if (id == promptRequestId_) {
        // Rows are written as "sent"; only failures rewrite the history file.
        promptRequestId_ = 0;
        pendingPromptText_ = "";
        pendingVoiceTranscript_ = "";
        status_ = "PROMPT ACCEPTED";
    } else if (id == sessionsRequestId_) {
        sessionsRequestId_ = 0;
        sessions_.clear();
        JsonArrayConst items = result["sessions"].as<JsonArrayConst>();
        if (items.isNull()) items = result["data"].as<JsonArrayConst>();
        if (items.isNull() && result.is<JsonArrayConst>()) items = result.as<JsonArrayConst>();
        if (!items.isNull() && cache_.enabled()) cache_.replaceSessions(items);
        for (JsonObjectConst item : items) {
            CachedSession session;
            session.id = item["session_id"] | item["id"] | "";
            session.title = item["title"] | item["name"] | "Untitled";
            session.preview = item["preview"] | "";
            session.state = item["status"] | item["state"] | "";
            if (session.id.length()) sessions_.push_back(session);
        }
        selectedSession_ = min(selectedSession_, max(0, static_cast<int>(sessions_.size()) - 1));
        sessionsWindowOffset_ = 0;
        sessionsTotal_ = sessions_.size();
        status_ = String(sessions_.size()) + " HERMES SESSIONS";
    } else if (id == createRequestId_) {
        const bool voiceFirst = pendingVoiceSession_;
        pendingVoiceSession_ = false;
        createRequestId_ = 0;
        activeSessionId_ = result["session_id"] | result["id"] | "";
        activeStoredSessionId_ =
            result["stored_session_id"] | activeSessionId_;
        cache_.protectSession(activeStoredSessionId_);
        activeSessionTitle_ = result["title"] | "New session";
        if (activeSessionId_.length()) {
            timeline_ = "";
            compose_ = "";
            if (voiceFirst) {
                screen_ = Screen::kChat;
                status_ = "STARTING VOICE";
                startVoice();
            } else {
                String cueError;
                (void)audioClient_.playUiCue(UiCue::kSessionOpen, cueError);
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
            String cueError;
            (void)audioClient_.playUiCue(UiCue::kAttention, cueError);
        } else if (!pendingClarify.isNull()) {
            interactionType_ = "clarify.request";
            interactionId_ = pendingClarify["request_id"] | "";
            prepareClarify(pendingClarify);
            compose_ = "";
            screen_ = Screen::kInteraction;
            status_ = "RESTORED QUESTION";
            String cueError;
            (void)audioClient_.playUiCue(UiCue::kAttention, cueError);
        } else if (retryVoiceAfterResume_ && voiceRetryAvailable_) {
            retryVoiceAfterResume_ = false;
            transcribeVoiceFile();
        } else if (pendingVoiceTranscript_.length()) {
            if (screen_ == Screen::kCompose &&
                compose_ == pendingVoiceTranscript_ &&
                submitText(pendingVoiceTranscript_)) {
                compose_ = "";
            } else if (screen_ != Screen::kCompose ||
                       compose_ != pendingVoiceTranscript_) {
                pendingVoiceTranscript_ = "";
            }
        } else {
            String cueError;
            (void)audioClient_.playUiCue(UiCue::kSessionOpen, cueError);
            if (timelineFromCache_) status_ = "LIVE / CACHED";
            else if (historySyncAttempted_) status_ = "LIVE / NO CACHE";
            else requestHistory();
        }
    } else if (id == branchRequestId_) {
        activeSessionId_ = result["session_id"] | "";
        activeStoredSessionId_ =
            result["stored_session_id"] | activeSessionId_;
        cache_.protectSession(activeStoredSessionId_);
        activeSessionTitle_ = result["title"] | "Branch";
        if (activeSessionId_.length()) {
            String cueError;
            (void)audioClient_.playUiCue(UiCue::kSessionOpen, cueError);
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
        if (!messages.isNull() && cache_.enabled())
            cache_.replaceHistory(activeStoredSessionId_, messages);
        for (JsonObjectConst item : messages) {
            const String role = item["role"] | item["type"] | "";
            String text = item["text"] | item["content"] | "";
            if (!text.length()) text = jsonText(item["content"]);
            if (text.length()) {
                appendTimeline((role == "user" ? "YOU: " : "HERMES: ") +
                               text + "\n\n");
                if (role == "assistant") {
                    lastAssistantText_ = text.length() > kMaxTtsText
                                             ? text.substring(0, kMaxTtsText)
                                             : text;
                }
            }
        }
        status_ = activeSessionTitle_;
        timelineFromCache_ = false;
    }
}

void App::parseEvent(JsonObjectConst params)
{
    const String type = params["type"] | "";
    JsonObjectConst payload = params["payload"].as<JsonObjectConst>();
    const String eventSession = params["session_id"] | "";
    if (eventSession.length()) {
        if (screen_ == Screen::kSessions) return;
        // Until session.resume answers, the runtime id is unknown; any other
        // session's stream must not be spooled into this session's cache.
        if (activeSessionId_.length() ? eventSession != activeSessionId_
                                      : type != "session.resume_progress") {
            return;
        }
    }

    if (type == "gateway.ready") {
        status_ = "HERMES READY";
    } else if (type == "message.start") {
        turnInProgress_ = true;
        currentAssistantText_ = "";
        lastInterimText_ = "";
        reasoningOpen_ = false;
        cache_.beginAssistantSpool(activeStoredSessionId_);
        status_ = "HERMES STARTING RESPONSE";
    } else if (type == "message.delta") {
        const String delta = payload["text"] | "";
        if (reasoningOpen_) {
            appendTimeline("\nHERMES: ");
            reasoningOpen_ = false;
        }
        appendTimeline(delta);
        cache_.appendAssistantDelta(delta);
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
        turnInProgress_ = false;
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
        if (cache_.finalizeAssistantSpool(completeText) && timelineFromCache_) {
            // The visible window still ends at the pre-turn byte offset. Mark
            // the upper bound unknown so forward paging reloads it from SD.
            cacheTotalBytes_ = static_cast<std::size_t>(-1);
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
        // Each cached refresh drops the socket for a REST sync, so bursts of
        // change events are coalesced into a hint instead.
        if (millis() - lastSessionsSyncMs_ < kSessionsRefreshDebounceMs) {
            status_ = "SESSIONS CHANGED - R REFRESH";
        } else {
            requestSessions();
        }
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
        String cueError;
        (void)audioClient_.playUiCue(UiCue::kAttention, cueError);
    } else if (type == "error") {
        turnInProgress_ = false;
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
    scroll_ = kScrollFollowBottom;
}

void App::serviceInput()
{
    if (M5Cardputer.BtnA.wasHold()) {
        if (voice_.active()) return;
        audioError_ = "";
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
    audioError_ = "";
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
        if (helpPage_ == 1 && (key == 'c' || key == 'C')) {
            if (!cacheClearConfirm_) {
                cacheClearConfirm_ = true;
                status_ = "PRESS C AGAIN TO CLEAR CACHE";
            } else {
                const bool cleared = cache_.clear();
                cacheClearConfirm_ = false;
                status_ = cleared ? "CACHE CLEARED" : "CACHE CLEAR FAILED";
            }
            dirty_ = true;
            return;
        }
        if (cacheClearConfirm_) cacheClearConfirm_ = false;
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
            settingRow_ = (settingRow_ + 7) % 8;
        } else if (key == '.') {
            settingRow_ = (settingRow_ + 1) % 8;
        } else if (key == '-' || key == '[' || key == ',') {
            adjustUiSetting(-1);
        } else if (key == '=' || key == ']' || key == '/' || key == '\n') {
            adjustUiSetting(1);
        }
    } else if (screen_ == Screen::kSessions) {
        if (key == '`' && sessionsSyncPending_) {
            hermes_.cancelRestDownload();
            status_ = "CANCELLING SESSION SYNC";
        }
        else if (key == ';' && !sessions_.empty()) {
            if (selectedSession_ > 0) {
                --selectedSession_;
            } else if (sessionsWindowOffset_ > 0) {
                sessionsWindowOffset_ = sessionsWindowOffset_ > 12
                                            ? sessionsWindowOffset_ - 12 : 0;
                cache_.loadSessions(sessions_, 12, sessionsWindowOffset_);
                selectedSession_ = max(0, static_cast<int>(sessions_.size()) - 1);
            }
        }
        else if (key == '.' && !sessions_.empty()) {
            if (selectedSession_ + 1 < static_cast<int>(sessions_.size())) {
                ++selectedSession_;
            } else if (sessionsWindowOffset_ + sessions_.size() < sessionsTotal_) {
                sessionsWindowOffset_ += sessions_.size();
                cache_.loadSessions(sessions_, 12, sessionsWindowOffset_);
                selectedSession_ = 0;
            }
        }
        else if (key == '\n') openSession();
        else if ((key == 'n' || key == 'N') && hermes_.connected() &&
                 !sessionsRequestId_ && !createRequestId_) createSession(false);
        else if ((key == 'v' || key == 'V') && hermes_.connected() &&
                 !sessionsRequestId_ && !createRequestId_) createSession(true);
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
        else if (key == '\b' && voiceRetryAvailable_) {
            SD.remove(kVoicePath);
            voiceRetryAvailable_ = false;
            status_ = "VOICE RECORDING DISCARDED";
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
        } else if (key == ';') {
            if (scroll_ > 0) {
                scroll_ = max(0, scroll_ - 3);
            } else if (timelineFromCache_ && cacheWindowStart_ > 0) {
                const std::size_t start = cacheWindowStart_ > 2304
                                              ? cacheWindowStart_ - 2304 : 0;
                if (cache_.loadTimelineWindow(
                        activeStoredSessionId_, start, timeline_,
                        lastAssistantText_, cacheWindowStart_, cacheWindowEnd_,
                        cacheTotalBytes_)) scroll_ = kScrollFollowBottom;
            }
        } else if (key == '.') {
            if (scroll_ < timelineMaxScroll_) {
                scroll_ += 3;
            } else if (timelineFromCache_ &&
                       cacheWindowEnd_ < cacheTotalBytes_) {
                const std::size_t start = cacheWindowEnd_ > 768
                                              ? cacheWindowEnd_ - 768 : 0;
                if (cache_.loadTimelineWindow(
                        activeStoredSessionId_, start, timeline_,
                        lastAssistantText_, cacheWindowStart_, cacheWindowEnd_,
                        cacheTotalBytes_)) scroll_ = 0;
            }
        }
    } else if (screen_ == Screen::kCompose) {
        if (key == '`') {
            pendingVoiceTranscript_ = "";
            screen_ = Screen::kChat;
        }
        else if ((key == 'v' || key == 'V') && !compose_.length()) startVoice();
        else if (key == '\n') submitCompose();
        else if (key == '\b' && compose_.length()) {
            pendingVoiceTranscript_ = "";
            compose_.remove(compose_.length() - 1);
        }
        else if (!keys.word.empty() && compose_.length() < 4000) {
            pendingVoiceTranscript_ = "";
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
    if (!compose_.length()) return;
    if (!activeSessionId_.length()) {
        if (pendingVoiceTranscript_.length() &&
            activeStoredSessionId_.length() && hermes_.connected() &&
            !resumeRequestId_) {
            JsonDocument params;
            params["session_id"] = activeStoredSessionId_;
            params["omit_messages"] = true;
            params["defer_history"] = true;
            if (config_.profile.length()) params["profile"] = config_.profile;
            resumeRequestId_ =
                hermes_.request("session.resume", params.as<JsonObjectConst>());
            status_ = resumeRequestId_ ? "RESTORING SESSION"
                                       : "SESSION RESTORE FAILED";
        } else {
            status_ = hermes_.connected() ? "SESSION NOT LIVE - WAIT"
                                          : "HERMES OFFLINE";
        }
        return;
    }
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
    if (turnInProgress_ || promptRequestId_) {
        status_ = "WAIT FOR HERMES RESPONSE";
        return false;
    }
    JsonDocument params;
    params["session_id"] = activeSessionId_;
    params["text"] = text;
    promptRequestId_ =
        hermes_.request("prompt.submit", params.as<JsonObjectConst>());
    if (!promptRequestId_) {
        status_ = "PROMPT SEND FAILED";
        return false;
    }
    pendingPromptText_ = text;
    if (cache_.appendMessage(activeStoredSessionId_, "user",
                             displayText.length() ? displayText : text,
                             "sent", String(promptRequestId_)) &&
        timelineFromCache_) {
        cacheTotalBytes_ = static_cast<std::size_t>(-1);
    }
    currentAssistantText_ = "";
    turnInProgress_ = true;
    appendTimeline("YOU: " + (displayText.length() ? displayText : text) +
                   "\n\nHERMES: ");
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
    audioError_ = "";
    if (historySyncPending_) {
        status_ = "WAIT FOR HISTORY SYNC";
        audioError_ = status_;
        return;
    }
    if (voice_.active() || turnInProgress_ || promptRequestId_ ||
        currentAssistantText_.length() || reasoningOpen_) {
        status_ = "WAIT FOR HERMES RESPONSE";
        audioError_ = status_;
        return;
    }
    if (!lastAssistantText_.length()) {
        status_ = "NO HERMES RESPONSE TO SPEAK";
        audioError_ = status_;
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
    HermesAudioClient::resetCancellation();
    if (!hermes_.refreshAuthentication(
            &HermesAudioClient::pollCancellation)) {
        screen_ = Screen::kChat;
        status_ = hermes_.diagnostic();
        audioError_ = status_;
        dirty_ = true;
        return;
    }
    String error;
    const bool spoken = audioClient_.speak(lastAssistantText_, kTtsPath, error);
    screen_ = Screen::kChat;
    status_ = spoken ? "SPEECH COMPLETE - RECONNECTING" : error;
    if (!spoken) audioError_ = error;
    dirty_ = true;
}

void App::startVoice()
{
    if (historySyncPending_) {
        status_ = "WAIT FOR HISTORY SYNC";
        return;
    }
    if (turnInProgress_ || promptRequestId_) {
        status_ = "WAIT FOR HERMES RESPONSE";
        return;
    }
    if (voiceRetryAvailable_) {
        if (!activeSessionId_.length()) {
            if (activeStoredSessionId_.length() && hermes_.connected() &&
                !resumeRequestId_) {
                JsonDocument params;
                params["session_id"] = activeStoredSessionId_;
                params["omit_messages"] = true;
                params["defer_history"] = true;
                if (config_.profile.length()) {
                    params["profile"] = config_.profile;
                }
                retryVoiceAfterResume_ = true;
                resumeRequestId_ = hermes_.request(
                    "session.resume", params.as<JsonObjectConst>());
                if (!resumeRequestId_) retryVoiceAfterResume_ = false;
                status_ = resumeRequestId_ ? "RESTORING VOICE SESSION"
                                           : "SESSION RESTORE FAILED";
            } else {
                status_ = "WAIT FOR HERMES - V RETRY";
            }
            return;
        }
        transcribeVoiceFile();
        return;
    }
    retryVoiceAfterResume_ = false;
    if (!activeSessionId_.length()) return;
    audioError_ = "";
    pendingVoiceTranscript_ = "";
    if (!voice_.begin(kVoicePath)) {
        status_ = voice_.error();
        audioError_ = status_;
        return;
    }
    screen_ = Screen::kRecording;
    status_ = "LISTENING";
    recordingFrameMs_ = 0;
}

void App::startVoiceTest()
{
    audioError_ = "";
    voiceRetryAvailable_ = false;
    if (!voice_.begin(kVoicePath)) {
        status_ = voice_.error();
        audioError_ = status_;
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
        if (submit && !complete) audioError_ = status_;
        dirty_ = true;
        return;
    }
    if (!submit) {
        voice_.cancel();
        screen_ = Screen::kChat;
        status_ = "VOICE CANCELLED";
        dirty_ = true;
        return;
    }
    status_ = "FINALIZING VOICE";
    dirty_ = true;
    draw();
    if (!voice_.finish()) {
        status_ = voice_.error();
        audioError_ = status_;
        screen_ = Screen::kChat;
        dirty_ = true;
        return;
    }
    transcribeVoiceFile();
}

void App::transcribeVoiceFile()
{
    status_ = "HERMES TRANSCRIBING";
    dirty_ = true;
    screen_ = Screen::kRecording;
    draw();
    // Free the WebSocket TLS context before opening the ticket and upload TLS
    // clients. Keeping both alive after capture can make the second TLS
    // allocation fail on the ESP32-S3.
    hermes_.disconnect();
    activeSessionId_ = "";
    delay(20);
    HermesAudioClient::resetCancellation();
    if (!hermes_.refreshAuthentication(
            &HermesAudioClient::pollCancellation)) {
        status_ = hermes_.diagnostic();
        audioError_ = status_;
        voiceRetryAvailable_ = SD.exists(kVoicePath);
        screen_ = Screen::kChat;
        dirty_ = true;
        return;
    }
    String transcript;
    String error;
    const bool transcribed =
        audioClient_.transcribeWav(kVoicePath, transcript, error);
    screen_ = Screen::kChat;
    if (!transcribed) {
        voiceRetryAvailable_ = SD.exists(kVoicePath);
        status_ = voiceRetryAvailable_ ? error + " / V RETRY" : error;
        audioError_ = status_;
        dirty_ = true;
        return;
    }
    SD.remove(kVoicePath);
    voiceRetryAvailable_ = false;
    // The TLS upload deliberately released the WebSocket. Keep the recognized
    // text visible, then submit it only after session.resume returns the new
    // runtime session id. Calling update() once here races that async response.
    pendingVoiceTranscript_ = transcript;
    compose_ = transcript;
    composeMode_ = ComposeMode::kPrompt;
    screen_ = Screen::kCompose;
    status_ = "TRANSCRIPT READY - SENDING";
    dirty_ = true;
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

}  // namespace hermes_terminal
