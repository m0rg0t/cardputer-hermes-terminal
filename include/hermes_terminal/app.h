#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <vector>

#include "hermes_terminal/config.h"
#include "hermes_terminal/hermes_client.h"
#include "hermes_terminal/hermes_audio_client.h"
#include "hermes_terminal/sd_cache.h"
#include "hermes_terminal/voice_capture.h"
#ifndef HERMES_WEB_ADMIN
#define HERMES_WEB_ADMIN 0
#endif
#if HERMES_WEB_ADMIN
#include "hermes_terminal/web_admin.h"
#endif

namespace hermes_terminal {

class App final : public HermesClientListener
#if HERMES_WEB_ADMIN
                , public WebAdminListener
#endif
{
public:
    void begin();
    void update();

    void onHermesConnected() override;
    void onHermesDisconnected(const String& reason) override;
    void onHermesMessage(JsonDocument& message) override;
    void onHermesAuthCookieUpdated(const String& cookie) override;
#if HERMES_WEB_ADMIN
    void writeWebStatus(JsonObject output) override;
    bool updateWebAuthCookie(const String& cookie) override;
    bool submitWebPrompt(const String& text) override;
    bool interruptWebSession() override;
#endif

#if defined(HERMES_SIM)
    // Desktop preview (sim/): scripts private state to render every screen.
    friend struct SimAccess;
#endif
private:
    enum class Screen : std::uint8_t {
        kSessions, kChat, kCompose, kInteraction, kRecording, kPlayback,
        kHelpSettings, kWifi
    };
    enum class WifiPhase : std::uint8_t {
        kScanning, kList, kPassword, kJoining
    };
    struct WifiNetwork {
        String ssid;
        std::int8_t rssi = 0;
        bool secured = false;
    };
    enum class ComposeMode : std::uint8_t { kPrompt, kSteer };
    enum class HistorySyncPhase : std::uint8_t { kNone, kLatest, kOldest };
    struct ClarifyQuestion { String id; String prompt; };

    bool mountSd();
    bool loadCa();
    void loadUiSettings();
    bool saveUiSettings();
    void adjustUiSetting(int delta);
    void loadAuthCookie();
    bool saveAuthCookie(const String& cookie);
    void startWifi();
    void serviceMdns();
    void serviceInput();
    void loadKnownWifi();
    bool saveKnownWifi();
    void rememberWifi(const String& ssid, const String& password);
    bool forgetWifi(const String& ssid);
    const WifiCredential* knownWifi(const String& ssid) const;
    void serviceWifiAuto();
    void openWifiSetup();
    void startWifiScan();
    void serviceWifiSetup();
    void joinWifi(const String& ssid, const String& password);
    void drawWifiScreen();
    void requestSessions();
    bool startSessionsPage(std::size_t offset);
    void createSession(bool voiceFirst = false);
    void openSession();
    void returnToSessions();
    void requestHistory();
    bool beginHistorySync();
    bool startHistoryPage(HistorySyncPhase phase, std::size_t offset);
    void serviceHistorySync();
    void submitCompose();
    bool submitText(const String& text, const String& displayText = "");
    void startCommand(const String& command, bool alias = false);
    void dispatchPendingCommand();
    void handleCommandResult(JsonVariantConst result);
    void startVoice();
    void startVoiceTest();
    void finishVoice(bool submit);
    void transcribeVoiceFile();
    void speakLastResponse();
    void respondInteraction(const String& value);
    void prepareClarify(JsonObjectConst payload);
    void updateUsage(JsonVariantConst value);
    void appendTimeline(const String& text);
    void parseResponse(JsonObjectConst root);
    void parseEvent(JsonObjectConst params);
    void serviceScreenSleep();
    void enterScreenSleep();
    void exitScreenSleep();
    bool canScreenSleep() const;
    void draw();
    void drawSessionsScreen();
    void drawRecordingScreen();
    void drawHelpSettingsScreen();
    void drawScreenSleep();
    String shortText(const String& value, std::size_t maxLength) const;

    Config config_;
    String caPem_;
    String status_ = "BOOTING";
    String lastHermesDiagnostic_;
    String audioError_;
    HermesClient hermes_;
    HermesAudioClient audioClient_;
    VoiceCapture voice_;
#if HERMES_WEB_ADMIN
    WebAdmin webAdmin_;
#endif
    SdCache cache_;
    Screen screen_ = Screen::kSessions;
    Screen helpReturnScreen_ = Screen::kSessions;
    std::vector<CachedSession> sessions_;
    int selectedSession_ = 0;
    String activeSessionId_;
    String activeStoredSessionId_;
    String activeSessionTitle_;
    String timeline_;
    String currentAssistantText_;
    String lastInterimText_;
    String lastAssistantText_;
    String usageText_;
    String compose_;
    ComposeMode composeMode_ = ComposeMode::kPrompt;
    int scroll_ = 0;
    String interactionType_;
    String interactionId_;
    String interactionPrompt_;
    String approvalChoices_ = ",once,session,deny,";
    std::vector<ClarifyQuestion> clarifyQuestions_;
    std::size_t clarifyQuestionIndex_ = 0;
    bool dirty_ = true;
    std::uint32_t sessionsRequestId_ = 0;
    bool sessionsSyncPending_ = false;
    bool sessionsFreshAfterRest_ = false;
    std::size_t sessionsSyncOffset_ = 0;
    std::size_t sessionsWindowOffset_ = 0;
    std::size_t sessionsTotal_ = 0;
    std::uint32_t createRequestId_ = 0;
    std::uint32_t promptRequestId_ = 0;
    std::uint32_t historyRequestId_ = 0;
    std::uint32_t resumeRequestId_ = 0;
    std::uint32_t cancelledResumeRequestId_ = 0;
    std::uint32_t branchRequestId_ = 0;
    std::uint32_t compressRequestId_ = 0;
    std::uint32_t undoRequestId_ = 0;
    std::uint32_t steerRequestId_ = 0;
    std::uint32_t slashRequestId_ = 0;
    std::uint32_t commandRequestId_ = 0;
    String pendingCommandName_;
    String pendingCommandArg_;
    String pendingCommandDisplay_;
    String pendingPromptText_;
    std::uint8_t commandAliasDepth_ = 0;
    bool reasoningOpen_ = false;
    bool turnInProgress_ = false;
    bool pendingVoiceSession_ = false;
    String pendingVoiceTranscript_;
    bool voiceRetryAvailable_ = false;
    bool retryVoiceAfterResume_ = false;
    std::uint8_t helpPage_ = 0;
    std::uint8_t settingRow_ = 0;
    std::uint8_t awakeBrightness_ = 150;
    std::uint8_t sleepMotion_ = 1;
    bool alertsEnabled_ = false;
    bool cacheEnabled_ = true;
    bool cacheReady_ = false;
    std::uint32_t lastSessionsSyncMs_ = 0;
    std::uint16_t cacheQuotaMb_ = 32;
    bool timelineFromCache_ = false;
    bool historySyncPending_ = false;
    bool historySyncAttempted_ = false;
    HistorySyncPhase historySyncPhase_ = HistorySyncPhase::kNone;
    std::size_t historySyncOffset_ = 0;
    std::size_t cacheWindowStart_ = 0;
    std::size_t cacheWindowEnd_ = 0;
    std::size_t cacheTotalBytes_ = 0;
    int timelineMaxScroll_ = 0;
    bool uiSettingsDirty_ = false;
    bool cacheClearConfirm_ = false;
    bool voiceTest_ = false;
    bool screenSleep_ = false;
    bool mdnsStarted_ = false;
    String mdnsName_;
    std::uint32_t lastInputMs_ = 0;
    std::uint32_t sleepFrameMs_ = 0;
    std::uint16_t sleepFrame_ = 0;
    std::uint32_t recordingFrameMs_ = 0;
    std::vector<WifiNetwork> wifiNetworks_;
    WifiPhase wifiPhase_ = WifiPhase::kList;
    int selectedWifi_ = 0;
    String wifiTargetSsid_;
    String wifiNotice_;
    std::uint32_t wifiJoinStartMs_ = 0;
    // Known networks: learned joins (most recent first) then HERMES.CFG.
    std::vector<WifiCredential> wifiLearned_;
    std::vector<WifiCredential> wifiKnown_;
    bool wifiAutoScanning_ = false;
    std::uint32_t wifiAutoScanMs_ = 0;
    std::uint8_t wifiAutoAttempt_ = 0;
    std::uint8_t wifiLastReason_ = 0;
    static const char* wifiReasonText(std::uint8_t reason);
};

}  // namespace hermes_terminal
