#pragma once

#include <Arduino.h>
#include <FS.h>
#include <M5Cardputer.h>

namespace hermes_terminal {

class VoiceCapture {
public:
    bool begin(const char* path);
    bool update();
    bool finish();
    void cancel();
    bool active() const { return active_; }
    unsigned long elapsedMs() const;
    std::uint8_t levelBars() const { return levelBars_; }
    String error() const { return error_; }

#if defined(HERMES_SIM)
    // Desktop preview (sim/): scripts private state to render every screen.
    friend struct SimAccess;
#endif
private:
    static constexpr std::size_t kBufferCount = 4;
    static constexpr std::size_t kQueueDepth = 2;
    static constexpr std::size_t kSamplesPerBuffer = 2048;
    static constexpr std::uint32_t kSampleRate = 16000;

    bool queueBuffer();
    bool serviceCompleted(bool refill);
    bool writeHeader(std::uint32_t dataBytes);
    void stopCodec();
    void setCodecMuted(bool muted);

    File file_;
    String path_;
    String error_;
    std::int16_t buffers_[kBufferCount][kSamplesPerBuffer] = {};
    bool inUse_[kBufferCount] = {};
    std::uint8_t activeQueue_[kQueueDepth] = {};
    std::size_t activeHead_ = 0;
    std::size_t activeCount_ = 0;
    std::uint32_t dataBytes_ = 0;
    std::uint8_t levelBars_ = 0;
    unsigned long startedMs_ = 0;
    bool active_ = false;
};

}  // namespace hermes_terminal
