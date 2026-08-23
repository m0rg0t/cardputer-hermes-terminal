#pragma once

#include <Arduino.h>
#include <FS.h>
#include <M5Cardputer.h>

#include "hermes_terminal/config.h"

namespace hermes_terminal {

enum class UiCue : std::uint8_t {
    kStartup,
    kConnected,
    kSessionOpen,
    kAttention,
};

class HermesAudioClient {
public:
    bool begin(const Config& config, const String& caPem);
    void setSessionCookie(const String& cookie) { config_.sessionCookie = cookie; }
    void setTtsVolume(std::uint8_t volume) { config_.ttsVolume = volume; }
    void setUiCuesEnabled(bool enabled) { uiCuesEnabled_ = enabled; }
    bool transcribeWav(const char* path, String& transcript, String& error);
    bool speak(const String& text, const char* path, String& error);
    bool playUiCue(UiCue cue, String& error);
    bool testSpeaker(String& error);

private:
    bool synthesize(const String& text, const char* path, String& mimeType,
                    String& error);
    bool play(const char* path, const String& mimeType, String& error);
    bool playMp3(File& file, String& error);
    bool playWav(File& file, String& error);
    bool beginSpeaker(String& error);
    void endSpeaker();

    Config config_;
    String caPem_;
    bool uiCuesEnabled_ = false;
};

}  // namespace hermes_terminal
