#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <vector>

#include "hermes_terminal/config.h"
#include "hermes_terminal/hermes_client.h"
#include "hermes_terminal/hermes_audio_client.h"
#include "hermes_terminal/voice_capture.h"
#include "hermes_terminal/web_admin.h"

namespace hermes_terminal {

class App final : public HermesClientListener, public WebAdminListener {
public:
    void begin();
    void update();

    void onHermesConnected() override;
    void onHermesDisconnected(const String& reason) override;
    void onHermesMessage(JsonDocument& message) override;
    void onHermesAuthCookieUpdated(const String& cookie) override;
    void writeWebStatus(JsonObject output) override;
    bool updateWebAuthCookie(const String& cookie) override;
    bool submitWebPrompt(const String& text) override;
    bool interruptWebSession() override;

private:
    enum class Screen : std::uint8_t {
        kSessions, kChat, kCompose, kInteraction, kRecording, kPlayback,
        kHelpSettings
    };
    enum class ComposeMode : std::uint8_t { kPrompt, kSteer };
    struct Session { String id; String title; String preview; String state; };
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
    void requestSessions();
    void createSession(bool voiceFirst = false);
    void openSession();
    void returnToSessions();
    void requestHistory();
    void submitCompose();
    bool submitText(const String& text, const String& displayText = "");
    void startCommand(const String& command, bool alias = false);
    void dispatchPendingCommand();
    void handleCommandResult(JsonVariantConst result);
    void startVoice();
    void startVoiceTest();
    void finishVoice(bool submit);
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
    HermesClient hermes_;
    HermesAudioClient audioClient_;
    VoiceCapture voice_;
    WebAdmin webAdmin_;
    Screen screen_ = Screen::kSessions;
    Screen helpReturnScreen_ = Screen::kSessions;
    std::vector<Session> sessions_;
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
    std::uint32_t createRequestId_ = 0;
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
    std::uint8_t commandAliasDepth_ = 0;
    bool reasoningOpen_ = false;
    bool pendingVoiceSession_ = false;
    String pendingVoiceTranscript_;
    std::uint8_t helpPage_ = 0;
    std::uint8_t settingRow_ = 0;
    std::uint8_t awakeBrightness_ = 150;
    std::uint8_t sleepMotion_ = 1;
    bool alertsEnabled_ = false;
    bool uiSettingsDirty_ = false;
    bool voiceTest_ = false;
    bool screenSleep_ = false;
    bool mdnsStarted_ = false;
    String mdnsName_;
    std::uint32_t lastInputMs_ = 0;
    std::uint32_t sleepFrameMs_ = 0;
    std::uint16_t sleepFrame_ = 0;
    std::uint32_t recordingFrameMs_ = 0;
};

}  // namespace hermes_terminal
