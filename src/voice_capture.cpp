#include "hermes_terminal/voice_capture.h"

#include <SD.h>
#include <cstring>

namespace hermes_terminal {
namespace {

constexpr std::uint8_t kEs8311Address = 0x18;
constexpr std::uint8_t kDacPowerRegister = 0x12;
constexpr std::uint8_t kDacMuteRegister = 0x31;
constexpr std::uint8_t kDacVolumeRegister = 0x32;
constexpr std::uint8_t kHeadphoneRegister = 0x13;
constexpr std::uint8_t kI2sBitClockPin = 41;
constexpr std::uint8_t kI2sDataOutPin = 42;
constexpr std::uint8_t kI2sWordSelectPin = 43;

void write16(std::uint8_t* out, std::uint16_t value)
{
    out[0] = value;
    out[1] = value >> 8;
}

void write32(std::uint8_t* out, std::uint32_t value)
{
    out[0] = value;
    out[1] = value >> 8;
    out[2] = value >> 16;
    out[3] = value >> 24;
}

}  // namespace

bool VoiceCapture::begin(const char* path)
{
    cancel();
    error_ = "";
    path_ = path;
    SD.remove(path);
    file_ = SD.open(path, FILE_WRITE);
    if (!file_ || !writeHeader(UINT32_MAX)) {
        error_ = "VOICE FILE FAILED";
        cancel();
        return false;
    }

    // Cardputer ADV's ES8311 needs its output clock path initialized before
    // the first ADC start after boot. Keep the DAC muted throughout capture.
    M5Cardputer.Speaker.setVolume(0);
    if (!M5Cardputer.Speaker.begin()) {
        error_ = "CODEC START FAILED";
        cancel();
        return false;
    }
    setCodecMuted(true);
    delay(60);
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    delay(40);

    auto config = M5Cardputer.Mic.config();
    config.magnification = 16;
    config.noise_filter_level = 0;
    M5Cardputer.Mic.config(config);
    if (!M5Cardputer.Mic.begin()) {
        error_ = "MIC START FAILED";
        cancel();
        return false;
    }
    delay(80);
    setCodecMuted(true);

    memset(inUse_, 0, sizeof(inUse_));
    activeHead_ = 0;
    activeCount_ = 0;
    dataBytes_ = 0;
    levelBars_ = 0;
    active_ = true;
    startedMs_ = millis();
    if (!queueBuffer() || !queueBuffer()) {
        error_ = "MIC QUEUE FAILED";
        cancel();
        return false;
    }
    return true;
}

bool VoiceCapture::queueBuffer()
{
    if (activeCount_ >= kQueueDepth) return false;
    std::size_t index = kBufferCount;
    for (std::size_t candidate = 0; candidate < kBufferCount; ++candidate) {
        if (!inUse_[candidate]) {
            index = candidate;
            break;
        }
    }
    if (index == kBufferCount ||
        !M5Cardputer.Mic.record(buffers_[index], kSamplesPerBuffer,
                               kSampleRate)) return false;
    inUse_[index] = true;
    activeQueue_[(activeHead_ + activeCount_) % kQueueDepth] = index;
    ++activeCount_;
    return true;
}

bool VoiceCapture::serviceCompleted(bool refill)
{
    const std::size_t hardwareQueued = M5Cardputer.Mic.isRecording();
    std::size_t completed = activeCount_ > hardwareQueued
                                ? activeCount_ - hardwareQueued
                                : 0;
    while (completed-- > 0) {
        const std::uint8_t index = activeQueue_[activeHead_];
        activeHead_ = (activeHead_ + 1) % kQueueDepth;
        --activeCount_;
        if (refill && !queueBuffer()) {
            error_ = "MIC QUEUE STARVED";
            return false;
        }

        // Add 6 dB and softly compress peaks before transcription. Track a
        // slow-decay peak for the on-screen hardware-style VU meter.
        std::int32_t peak = 0;
        for (std::size_t sample = 0; sample < kSamplesPerBuffer; ++sample) {
            std::int32_t value = static_cast<std::int32_t>(buffers_[index][sample]) * 2;
            const bool negative = value < 0;
            std::int32_t magnitude = negative ? -value : value;
            if (magnitude > 24500) magnitude = 24500 + (magnitude - 24500) / 4;
            magnitude = min<std::int32_t>(magnitude, 32767);
            peak = max(peak, magnitude);
            buffers_[index][sample] = negative ? -magnitude : magnitude;
        }
        const std::uint8_t measured = min<std::int32_t>(12, peak / 2300);
        levelBars_ = measured >= levelBars_ ? measured
                                            : max<std::uint8_t>(measured, levelBars_ - 1);
        const std::size_t bytes = sizeof(buffers_[index]);
        if (file_.write(reinterpret_cast<std::uint8_t*>(buffers_[index]), bytes) != bytes) {
            error_ = "VOICE SD WRITE FAILED";
            return false;
        }
        dataBytes_ += bytes;
        inUse_[index] = false;
    }
    return true;
}

bool VoiceCapture::update()
{
    return !active_ || serviceCompleted(true);
}

bool VoiceCapture::finish()
{
    if (!active_) return false;
    bool drained = true;
    const unsigned long deadline = millis() + 1500;
    while (activeCount_ > 0 && millis() < deadline) {
        if (!serviceCompleted(false)) {
            drained = false;
            break;
        }
        if (activeCount_) delay(1);
    }
    if (activeCount_) {
        drained = false;
        if (!error_.length()) error_ = "VOICE FINALIZE TIMEOUT";
    }
    stopCodec();
    active_ = false;
    const bool valid = drained && !error_.length() &&
                       dataBytes_ >= kSampleRate * sizeof(std::int16_t) / 4;
    if (drained && !error_.length() && !valid)
        error_ = "VOICE CLIP TOO SHORT";
    if (valid && file_.seek(0) && writeHeader(dataBytes_)) {
        file_.flush();
        file_.close();
        return true;
    }
    file_.close();
    SD.remove(path_);
    if (!error_.length()) error_ = "VOICE FINALIZE FAILED";
    return false;
}

void VoiceCapture::cancel()
{
    // A failed M5 speaker/mic begin may already have changed ES8311 registers
    // while isRunning() is still false. Always force the hardware quiet state.
    stopCodec();
    active_ = false;
    if (file_) file_.close();
    if (path_.length()) SD.remove(path_);
}

bool VoiceCapture::writeHeader(std::uint32_t dataBytes)
{
    std::uint8_t header[44] = {};
    memcpy(header, "RIFF", 4);
    write32(header + 4, dataBytes == UINT32_MAX ? UINT32_MAX : 36 + dataBytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    write32(header + 16, 16);
    write16(header + 20, 1);
    write16(header + 22, 1);
    write32(header + 24, kSampleRate);
    write32(header + 28, kSampleRate * 2);
    write16(header + 32, 2);
    write16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    write32(header + 40, dataBytes);
    return file_.write(header, sizeof(header)) == sizeof(header);
}

void VoiceCapture::stopCodec()
{
    setCodecMuted(true);
    M5Cardputer.Mic.end();
    delay(10);
    // Mic.end() runs the Cardputer ADV ES8311 reset/power-down callback after
    // our first mute writes. Reassert the DAC/HP shutdown afterward; otherwise
    // the unclocked analog output can remain audible as a low hum.
    M5Cardputer.Speaker.setVolume(0);
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    setCodecMuted(true);
    pinMode(kI2sBitClockPin, OUTPUT);
    pinMode(kI2sDataOutPin, OUTPUT);
    pinMode(kI2sWordSelectPin, OUTPUT);
    digitalWrite(kI2sBitClockPin, LOW);
    digitalWrite(kI2sDataOutPin, LOW);
    digitalWrite(kI2sWordSelectPin, LOW);
    delay(20);
}

void VoiceCapture::setCodecMuted(bool muted)
{
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacMuteRegister,
                                      muted ? 0x60 : 0x00, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacVolumeRegister,
                                      muted ? 0x00 : 0xBF, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kHeadphoneRegister,
                                      muted ? 0x00 : 0x10, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacPowerRegister,
                                      muted ? 0x02 : 0x00, 100000);
}

unsigned long VoiceCapture::elapsedMs() const
{
    return active_ ? millis() - startedMs_ : 0;
}

}  // namespace hermes_terminal
