#include "hermes_terminal/hermes_audio_client.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <cstring>

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3.h"

namespace hermes_terminal {
namespace {

constexpr unsigned long kAudioHttpTimeoutMs = 600000;
constexpr unsigned long kAudioWriteStallMs = 15000;
constexpr unsigned long kEarlyResponseWaitMs = 5000;
constexpr std::size_t kMaxTtsAudioBytes = 8U * 1024U * 1024U;
constexpr std::uint8_t kEs8311Address = 0x18;
constexpr std::uint8_t kDacPowerRegister = 0x12;
constexpr std::uint8_t kDacMuteRegister = 0x31;
constexpr std::uint8_t kDacVolumeRegister = 0x32;
constexpr std::uint8_t kHeadphoneRegister = 0x13;
constexpr std::uint8_t kI2sBitClockPin = 41;
constexpr std::uint8_t kI2sDataOutPin = 42;
constexpr std::uint8_t kI2sWordSelectPin = 43;
bool audioCancelled = false;

bool pollAudioCancel()
{
    if (audioCancelled) return true;
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange() ||
        !M5Cardputer.Keyboard.isPressed()) return false;
    const auto& keys = M5Cardputer.Keyboard.keysState();
    if (!keys.word.empty() && keys.word[0] == '`') {
        audioCancelled = true;
        return true;
    }
    return false;
}

bool parseHttpsUrl(const String& url, String& host, std::uint16_t& port,
                   String& basePath)
{
    if (!url.startsWith("https://")) return false;
    String authority = url.substring(8);
    const int slash = authority.indexOf('/');
    basePath = slash >= 0 ? authority.substring(slash) : "";
    if (slash >= 0) authority = authority.substring(0, slash);
    const int colon = authority.lastIndexOf(':');
    if (colon > 0) {
        host = authority.substring(0, colon);
        port = authority.substring(colon + 1).toInt();
    } else {
        host = authority;
        port = 443;
    }
    return host.length() && port;
}

String urlEncode(const String& value)
{
    static const char hex[] = "0123456789ABCDEF";
    String result;
    result.reserve(value.length() * 3);
    for (std::size_t index = 0; index < value.length(); ++index) {
        const std::uint8_t c = static_cast<std::uint8_t>(value[index]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0x0F];
        }
    }
    return result;
}

String audioRoute(const String& basePath, const char* endpoint,
                  const Config& config)
{
    String route = basePath + endpoint;
    if (config.profile.length()) {
        route += "?profile=" + urlEncode(config.profile);
    }
    return route;
}

void writeAuthHeader(WiFiClientSecure& client, const Config& config)
{
    if (config.sessionCookie.length()) {
        client.print("Cookie: " + config.sessionCookie + "\r\n");
    } else if (config.sessionToken.length()) {
        client.print("X-Hermes-Session-Token: " + config.sessionToken +
                     "\r\n");
    }
}

bool writeAll(WiFiClientSecure& client, const std::uint8_t* data,
              std::size_t length)
{
    std::size_t offset = 0;
    unsigned long lastProgress = millis();
    while (offset < length) {
        if (pollAudioCancel()) return false;
        const std::size_t written = client.write(data + offset,
                                                 length - offset);
        if (written) {
            offset += written;
            lastProgress = millis();
        } else {
            if (!client.connected() ||
                millis() - lastProgress >= kAudioWriteStallMs) return false;
            delay(2);
        }
        yield();
    }
    return true;
}

class AudioJsonStream final : public Stream {
public:
    explicit AudioJsonStream(File file) : file_(file)
    {
        audioSize_ = file_.size();
        totalSize_ = prefix_.length() + ((audioSize_ + 2) / 3) * 4 + suffix_.length();
    }

    int available() override
    {
        const std::size_t remaining = totalSize_ - position_;
        return remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
    }
    int read() override
    {
        if (peeked_ >= 0) {
            const int value = peeked_;
            peeked_ = -1;
            ++position_;
            return value;
        }
        const int value = next();
        if (value >= 0) ++position_;
        return value;
    }
    int peek() override
    {
        if (peeked_ < 0) peeked_ = next();
        return peeked_;
    }
    void flush() override {}
    std::size_t write(std::uint8_t) override { return 0; }
    std::size_t totalSize() const { return totalSize_; }

private:
    int next()
    {
        const std::size_t logical = position_;
        if (logical >= totalSize_) return -1;
        if (logical < prefix_.length()) return prefix_[logical];
        const std::size_t encodedEnd = totalSize_ - suffix_.length();
        if (logical >= encodedEnd) return suffix_[logical - encodedEnd];
        if (encodedOffset_ >= encodedLength_) {
            std::uint8_t input[192];
            const std::size_t count = file_.read(input, sizeof(input));
            if (!count) return -1;
            std::size_t written = 0;
            if (mbedtls_base64_encode(encoded_, sizeof(encoded_), &written,
                                      input, count) != 0) return -1;
            encodedOffset_ = 0;
            encodedLength_ = written;
        }
        return encoded_[encodedOffset_++];
    }

    File file_;
    const String prefix_ = "{\"data_url\":\"data:audio/wav;base64,";
    const String suffix_ = "\",\"mime_type\":\"audio/wav\"}";
    std::size_t audioSize_ = 0;
    std::size_t totalSize_ = 0;
    std::size_t position_ = 0;
    std::uint8_t encoded_[256] = {};
    std::size_t encodedOffset_ = 0;
    std::size_t encodedLength_ = 0;
    int peeked_ = -1;
};

class HttpBodyReader {
public:
    HttpBodyReader(WiFiClientSecure& client, bool chunked, int contentLength)
        : client_(client), chunked_(chunked), remaining_(contentLength) {}

    int read()
    {
        if ((bytesRead_++ & 0x1ffU) == 0 && pollAudioCancel()) return -1;
        if (!chunked_) {
            if (remaining_ == 0) return -1;
            const int value = readWire();
            if (value >= 0 && remaining_ > 0) --remaining_;
            return value;
        }
        if (finished_) return -1;
        if (chunkRemaining_ == 0) {
            if (needChunkCrlf_) {
                if (readWire() != '\r' || readWire() != '\n') return -1;
                needChunkCrlf_ = false;
            }
            String line;
            int value = -1;
            while ((value = readWire()) >= 0) {
                if (value == '\r') {
                    if (readWire() != '\n') return -1;
                    break;
                }
                if (line.length() < 24) line += static_cast<char>(value);
            }
            const int extension = line.indexOf(';');
            if (extension >= 0) line.remove(extension);
            chunkRemaining_ = strtoul(line.c_str(), nullptr, 16);
            if (!chunkRemaining_) {
                finished_ = true;
                return -1;
            }
        }
        const int value = readWire();
        if (value < 0) return -1;
        if (--chunkRemaining_ == 0) needChunkCrlf_ = true;
        return value;
    }

private:
    int readWire()
    {
        const unsigned long started = millis();
        unsigned long lastCancelPoll = 0;
        while (!client_.available()) {
            const unsigned long now = millis();
            if (now - lastCancelPoll >= 10) {
                lastCancelPoll = now;
                if (pollAudioCancel()) return -1;
            }
            if ((!client_.connected() && !client_.available()) ||
                now - started >= kAudioHttpTimeoutMs) return -1;
            delay(2);
        }
        return client_.read();
    }

    WiFiClientSecure& client_;
    bool chunked_ = false;
    int remaining_ = -1;
    std::size_t chunkRemaining_ = 0;
    bool needChunkCrlf_ = false;
    bool finished_ = false;
    std::size_t bytesRead_ = 0;
};

String readBody(HttpBodyReader& body, std::size_t limit)
{
    String result;
    result.reserve(min<std::size_t>(limit, 1024));
    int value = -1;
    while (result.length() < limit && (value = body.read()) >= 0)
        result += static_cast<char>(value);
    return result;
}

String httpFailure(const char* operation, int status, const String& response)
{
    String error(operation);
    error += " HTTP ";
    error += status;
    // The gateway reports provider failures as {"detail": "..."}; show
    // that text (e.g. a missing TTS/STT provider) instead of raw JSON.
    String message = response;
    int start = response.indexOf("\"detail\":\"");
    if (start >= 0) {
        start += 10;
        const int end = response.indexOf('"', start);
        if (end > start) message = response.substring(start, end);
    }
    if (message.length()) {
        error += ": ";
        for (std::size_t index = 0;
             index < message.length() && error.length() < 80; ++index) {
            const char value = message[index];
            error += value == '\r' || value == '\n' || value == '\t'
                         ? ' ' : value;
        }
    }
    return error;
}

bool readHttpHeaders(WiFiClientSecure& client, int& status, bool& chunked,
                     int& contentLength,
                     unsigned long timeoutMs = kAudioHttpTimeoutMs)
{
    String headers;
    const unsigned long started = millis();
    unsigned long lastCancelPoll = 0;
    while (headers.indexOf("\r\n\r\n") < 0) {
        const unsigned long now = millis();
        if (now - lastCancelPoll >= 10) {
            lastCancelPoll = now;
            if (pollAudioCancel()) return false;
        }
        if (client.available()) {
            headers += static_cast<char>(client.read());
            if (headers.length() > 4096) return false;
        } else if ((!client.connected() && !client.available()) ||
                   now - started >= timeoutMs) {
            return false;
        } else {
            delay(2);
        }
    }
    const int statusSpace = headers.indexOf(' ');
    status = statusSpace >= 0
                 ? headers.substring(statusSpace + 1, statusSpace + 4).toInt()
                 : 0;
    headers.toLowerCase();
    chunked = headers.indexOf("transfer-encoding: chunked") >= 0;
    contentLength = -1;
    const int lengthAt = headers.indexOf("content-length:");
    if (lengthAt >= 0) {
        contentLength = headers.substring(lengthAt + 15).toInt();
    }
    return true;
}

std::uint16_t read16(File& file)
{
    std::uint8_t bytes[2] = {};
    return file.read(bytes, sizeof(bytes)) == sizeof(bytes)
               ? bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8)
               : 0;
}

std::uint32_t read32(File& file)
{
    std::uint8_t bytes[4] = {};
    return file.read(bytes, sizeof(bytes)) == sizeof(bytes)
               ? bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16) |
                     (static_cast<std::uint32_t>(bytes[3]) << 24)
               : 0;
}

}  // namespace

void HermesAudioClient::resetCancellation()
{
    audioCancelled = false;
}

bool HermesAudioClient::pollCancellation()
{
    return pollAudioCancel();
}

bool HermesAudioClient::begin(const Config& config, const String& caPem)
{
    config_ = config;
    // Audio requests use only the short-lived session cookie. Keep the
    // long-lived dashboard password out of this secondary client copy.
    config_.loginPassword = "";
    caPem_ = caPem;
    return config_.baseUrl.startsWith("https://") && caPem_.length() > 0;
}

bool HermesAudioClient::transcribeWav(const char* path, String& transcript,
                                      String& error)
{
    transcript = "";
    error = "";
    if (pollAudioCancel()) {
        error = "TRANSCRIBE CANCELLED";
        return false;
    }
    File file = SD.open(path, FILE_READ);
    if (!file || file.size() <= 44) {
        error = "VOICE FILE EMPTY";
        return false;
    }
    AudioJsonStream body(file);
    String host;
    String basePath;
    std::uint16_t port = 443;
    if (!parseHttpsUrl(config_.baseUrl, host, port, basePath)) {
        error = "TRANSCRIBE URL FAILED";
        return false;
    }
    WiFiClientSecure client;
    client.setCACert(caPem_.c_str());
    client.setTimeout(kAudioHttpTimeoutMs / 1000);
    client.setHandshakeTimeout(5);
    if (!client.connect(host.c_str(), port, 5000)) {
        error = pollAudioCancel() ? "TRANSCRIBE CANCELLED"
                                  : "TRANSCRIBE TLS FAILED";
        return false;
    }
    // Note: connect(..., 5000) fixes the TLS write stall budget at 5 s for
    // this connection; the core does not re-read setTimeout after connect.
    // Reads are non-blocking, so the long response wait is enforced by our
    // own read loops instead.
    if (pollAudioCancel()) {
        client.stop();
        error = "TRANSCRIBE CANCELLED";
        return false;
    }
    const String route =
        audioRoute(basePath, "/api/audio/transcribe", config_);
    client.print("POST " + route + " HTTP/1.1\r\n");
    client.print("Host: " + host + ":" + String(port) + "\r\n");
    client.print("Content-Type: application/json\r\nAccept-Encoding: identity\r\n");
    client.print("Connection: close\r\nContent-Length: " +
                 String(body.totalSize()) + "\r\n");
    writeAuthHeader(client, config_);
    client.print("\r\n");
    std::uint8_t buffer[512];
    bool uploaded = true;
    while (body.available() > 0) {
        std::size_t count = 0;
        while (count < sizeof(buffer) && body.available() > 0) {
            const int value = body.read();
            if (value < 0) break;
            buffer[count++] = value;
        }
        if (!count || !writeAll(client, buffer, count)) {
            uploaded = false;
            break;
        }
        yield();
    }
    if (!uploaded && !client.connected() && !client.available()) {
        error = audioCancelled ? "TRANSCRIBE CANCELLED"
                               : "TRANSCRIBE UPLOAD FAILED";
        client.stop();
        return false;
    }

    int status = 0;
    int contentLength = -1;
    bool chunked = false;
    if (!readHttpHeaders(client, status, chunked, contentLength,
                         uploaded ? kAudioHttpTimeoutMs
                                  : kEarlyResponseWaitMs)) {
        error = audioCancelled ? "TRANSCRIBE CANCELLED"
                               : uploaded ? "TRANSCRIBE TIMEOUT"
                                          : "TRANSCRIBE UPLOAD FAILED";
        client.stop();
        return false;
    }
    HttpBodyReader bodyReader(client, chunked, contentLength);
    String response = readBody(bodyReader, 8192);
    client.stop();
    if (audioCancelled) {
        error = "TRANSCRIBE CANCELLED";
        return false;
    }
    if (status != 200) {
        error = httpFailure("TRANSCRIBE", status, response);
        return false;
    }
    JsonDocument document;
    if (deserializeJson(document, response)) {
        error = "TRANSCRIBE INVALID JSON";
        return false;
    }
    transcript = document["transcript"] | document["text"] | "";
    transcript.trim();
    if (!(document["ok"] | true) || !transcript.length()) {
        error = document["error"] | "EMPTY TRANSCRIPT";
        return false;
    }
    return true;
}

bool HermesAudioClient::speak(const String& text, const char* path,
                              String& error)
{
    String mimeType;
    SD.remove(path);
    if (!synthesize(text, path, mimeType, error)) {
        SD.remove(path);
        return false;
    }
    const bool played = play(path, mimeType, error);
    SD.remove(path);
    return played;
}

bool HermesAudioClient::synthesize(const String& text, const char* path,
                                   String& mimeType, String& error)
{
    error = "";
    JsonDocument document;
    document["text"] = text;
    String requestBody;
    serializeJson(document, requestBody);

    String host;
    String basePath;
    std::uint16_t port = 443;
    if (!parseHttpsUrl(config_.baseUrl, host, port, basePath)) {
        error = "TTS URL FAILED";
        return false;
    }
    WiFiClientSecure client;
    client.setCACert(caPem_.c_str());
    client.setTimeout(kAudioHttpTimeoutMs / 1000);
    client.setHandshakeTimeout(5);
    if (pollAudioCancel()) {
        error = "TTS CANCELLED";
        return false;
    }
    if (!client.connect(host.c_str(), port, 5000)) {
        error = pollAudioCancel() ? "TTS CANCELLED" : "TTS TLS FAILED";
        return false;
    }
    if (pollAudioCancel()) {
        client.stop();
        error = "TTS CANCELLED";
        return false;
    }
    client.print("POST " + audioRoute(basePath, "/api/audio/speak", config_) +
                 " HTTP/1.1\r\n");
    client.print("Host: " + host + ":" + String(port) + "\r\n");
    client.print("Content-Type: application/json\r\nAccept-Encoding: identity\r\n");
    client.print("Connection: close\r\nContent-Length: " +
                 String(requestBody.length()) + "\r\n");
    writeAuthHeader(client, config_);
    client.print("\r\n");
    const bool uploaded = writeAll(
        client, reinterpret_cast<const std::uint8_t*>(requestBody.c_str()),
        requestBody.length());
    if (!uploaded && !client.connected() && !client.available()) {
        error = audioCancelled ? "TTS CANCELLED" : "TTS UPLOAD FAILED";
        client.stop();
        return false;
    }

    int status = 0;
    int contentLength = -1;
    bool chunked = false;
    if (!readHttpHeaders(client, status, chunked, contentLength,
                         uploaded ? kAudioHttpTimeoutMs
                                  : kEarlyResponseWaitMs)) {
        error = audioCancelled ? "TTS CANCELLED"
                               : uploaded ? "TTS TIMEOUT"
                                          : "TTS UPLOAD FAILED";
        client.stop();
        return false;
    }
    HttpBodyReader body(client, chunked, contentLength);
    if (status != 200) {
        error = httpFailure("TTS", status, readBody(body, 1024));
        client.stop();
        return false;
    }
    const char token[] = "\"data_url\"";
    std::size_t matched = 0;
    int value = -1;
    while ((value = body.read()) >= 0 && matched < sizeof(token) - 1) {
        matched = value == token[matched] ? matched + 1
                                          : (value == token[0] ? 1 : 0);
    }
    if (audioCancelled) {
        error = "TTS CANCELLED";
        client.stop();
        return false;
    }
    if (matched != sizeof(token) - 1) {
        error = "TTS DATA MISSING";
        client.stop();
        return false;
    }
    while ((value = body.read()) >= 0 && value != ':') {}
    while ((value = body.read()) >= 0 && value != '"') {}
    if (value < 0) {
        error = audioCancelled ? "TTS CANCELLED" : "TTS INVALID JSON";
        client.stop();
        return false;
    }
    String prefix;
    while ((value = body.read()) >= 0 && value != ',' && value != '"') {
        if (prefix.length() < 96) prefix += static_cast<char>(value);
    }
    if (value != ',' || !prefix.startsWith("data:") ||
        prefix.indexOf(";base64") < 0) {
        error = audioCancelled ? "TTS CANCELLED" : "TTS INVALID DATA URL";
        client.stop();
        return false;
    }
    mimeType = prefix.substring(5, prefix.indexOf(';'));
    File output = SD.open(path, FILE_WRITE);
    if (!output) {
        error = "TTS SD FILE FAILED";
        client.stop();
        return false;
    }
    std::uint8_t encoded[256];
    std::uint8_t decoded[192];
    std::size_t encodedLength = 0;
    std::size_t totalWritten = 0;
    bool closed = false;
    auto flushDecoded = [&]() -> bool {
        if (!encodedLength) return true;
        std::size_t decodedLength = 0;
        if (encodedLength % 4 ||
            mbedtls_base64_decode(decoded, sizeof(decoded), &decodedLength,
                                  encoded, encodedLength) != 0 ||
            totalWritten + decodedLength > kMaxTtsAudioBytes ||
            output.write(decoded, decodedLength) != decodedLength) {
            return false;
        }
        totalWritten += decodedLength;
        encodedLength = 0;
        yield();
        return true;
    };
    while ((value = body.read()) >= 0) {
        if (value == '"') {
            closed = true;
            break;
        }
        if (value == '\r' || value == '\n' || value == ' ') continue;
        encoded[encodedLength++] = static_cast<std::uint8_t>(value);
        if (encodedLength == sizeof(encoded)) {
            if (pollAudioCancel()) {
                error = "TTS CANCELLED";
                output.close();
                client.stop();
                return false;
            }
            if (!flushDecoded()) {
                error = "TTS DECODE FAILED";
                output.close();
                client.stop();
                return false;
            }
        }
    }
    if (closed && !flushDecoded()) error = "TTS DECODE FAILED";
    output.flush();
    output.close();
    client.stop();
    if (audioCancelled) error = "TTS CANCELLED";
    if (!closed || error.length() || totalWritten < 16) {
        if (!error.length()) error = "TTS AUDIO TRUNCATED";
        return false;
    }
    return true;
}

bool HermesAudioClient::beginSpeaker(String& error)
{
    if (M5Cardputer.Mic.isRunning()) M5Cardputer.Mic.end();
    auto speakerConfig = M5Cardputer.Speaker.config();
    speakerConfig.magnification = 32;
    M5Cardputer.Speaker.config(speakerConfig);
    M5Cardputer.Speaker.setVolume(config_.ttsVolume);
    if (!M5Cardputer.Speaker.begin()) {
        error = "SPEAKER START FAILED";
        endSpeaker();
        return false;
    }
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacVolumeRegister,
                                      0xBF, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kHeadphoneRegister,
                                      0x10, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacPowerRegister,
                                      0x00, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacMuteRegister,
                                      0x00, 100000);
    delay(20);
    return true;
}

bool HermesAudioClient::endSpeaker(bool pollCancel)
{
    const unsigned long deadline = millis() + 3000;
    while (!audioCancelled && M5Cardputer.Speaker.isPlaying() &&
           millis() < deadline) {
        // Short interface cues run inside hermes_.update(); polling the
        // keyboard there would consume the keystroke before App sees it.
        if (pollCancel) pollAudioCancel();
        delay(2);
    }
    const bool drained = !M5Cardputer.Speaker.isPlaying();
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacMuteRegister,
                                      0x60, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacVolumeRegister,
                                      0x00, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kHeadphoneRegister,
                                      0x00, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacPowerRegister,
                                      0x02, 100000);
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    // M5Unified's Cardputer ADV speaker-disable callback is intentionally
    // empty, so enforce the quiet hardware state after I2S is uninstalled.
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacMuteRegister,
                                      0x60, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacVolumeRegister,
                                      0x00, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kHeadphoneRegister,
                                      0x00, 100000);
    M5Cardputer.In_I2C.writeRegister8(kEs8311Address, kDacPowerRegister,
                                      0x02, 100000);
    pinMode(kI2sBitClockPin, OUTPUT);
    pinMode(kI2sDataOutPin, OUTPUT);
    pinMode(kI2sWordSelectPin, OUTPUT);
    digitalWrite(kI2sBitClockPin, LOW);
    digitalWrite(kI2sDataOutPin, LOW);
    digitalWrite(kI2sWordSelectPin, LOW);
    return drained && !audioCancelled;
}

bool HermesAudioClient::testSpeaker(String& error)
{
    if (!uiCuesEnabled_) {
        error = "AUDIO ALERTS OFF";
        return false;
    }
    resetCancellation();
    if (!beginSpeaker(error)) return false;
    const bool started = M5Cardputer.Speaker.tone(880.0f, 100);
    if (!started) error = "SPEAKER TEST FAILED";
    const bool finished = endSpeaker();
    if (started && !finished) {
        error = audioCancelled ? "SPEAKER TEST CANCELLED"
                               : "SPEAKER TEST TIMEOUT";
    }
    return started && finished;
}

bool HermesAudioClient::playUiCue(UiCue cue, String& error)
{
    error = "";
    if (!uiCuesEnabled_) return true;
    resetCancellation();
    if (!beginSpeaker(error)) return false;
    // Interface cues stay gentler than spoken TTS. Each transition has a
    // compact signature, while the master UI-cue gate keeps silent mode quiet.
    M5Cardputer.Speaker.setVolume(config_.ttsVolume < 96
                                      ? config_.ttsVolume : 96);
    bool started = false;
    if (cue == UiCue::kStartup) {
        started = M5Cardputer.Speaker.tone(523.25f, 60);
        if (started) {
            delay(72);
            started = M5Cardputer.Speaker.tone(659.25f, 80);
        }
    } else if (cue == UiCue::kConnected) {
        started = M5Cardputer.Speaker.tone(783.99f, 65);
    } else if (cue == UiCue::kSessionOpen) {
        started = M5Cardputer.Speaker.tone(659.25f, 45);
        if (started) {
            delay(56);
            started = M5Cardputer.Speaker.tone(880.0f, 70);
        }
    } else {
        started = M5Cardputer.Speaker.tone(880.0f, 85);
    }
    if (!started) error = "UI SOUND FAILED";
    const bool finished = endSpeaker(false);
    if (started && !finished) {
        error = audioCancelled ? "UI SOUND CANCELLED" : "UI SOUND TIMEOUT";
    }
    return started && finished;
}

bool HermesAudioClient::play(const char* path, const String& mimeType,
                             String& error)
{
    File file = SD.open(path, FILE_READ);
    if (!file) {
        error = "TTS AUDIO FILE FAILED";
        return false;
    }
    std::uint8_t magic[12] = {};
    const std::size_t magicLength = file.read(magic, sizeof(magic));
    file.seek(0);
    const bool wav = magicLength == sizeof(magic) &&
                     memcmp(magic, "RIFF", 4) == 0 &&
                     memcmp(magic + 8, "WAVE", 4) == 0;
    const bool mp3 = mimeType == "audio/mpeg" || mimeType == "audio/mp3" ||
                     (magicLength >= 3 && memcmp(magic, "ID3", 3) == 0) ||
                     (magicLength >= 2 && magic[0] == 0xFF &&
                      (magic[1] & 0xE0) == 0xE0);
    if (!wav && !mp3) {
        error = "TTS CODEC UNSUPPORTED: " + mimeType;
        file.close();
        return false;
    }
    if (!beginSpeaker(error)) {
        file.close();
        return false;
    }
    const bool result = wav ? playWav(file, error) : playMp3(file, error);
    const bool finished = endSpeaker();
    file.close();
    if (result && !finished) {
        error = audioCancelled ? "SPEECH CANCELLED"
                               : "SPEAKER DRAIN TIMEOUT";
    }
    return result && finished;
}

bool HermesAudioClient::playMp3(File& file, String& error)
{
    static std::uint8_t input[16 * 1024];
    static std::int16_t pcm[2][MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_t decoder;
    mp3dec_init(&decoder);
    std::size_t buffered = 0;
    std::uint8_t pcmIndex = 0;
    bool decodedAny = false;
    while (file.available() || buffered) {
        if (pollAudioCancel()) {
            error = "SPEECH CANCELLED";
            return false;
        }
        if (file.available() && buffered < sizeof(input)) {
            buffered += file.read(input + buffered, sizeof(input) - buffered);
        }
        mp3dec_frame_info_t info = {};
        const int samples = mp3dec_decode_frame(&decoder, input, buffered,
                                                pcm[pcmIndex], &info);
        if (info.frame_bytes > 0) {
            buffered -= info.frame_bytes;
            memmove(input, input + info.frame_bytes, buffered);
        } else if (!file.available()) {
            break;
        } else if (buffered == sizeof(input)) {
            memmove(input, input + 1, --buffered);
        }
        if (samples > 0 && info.channels >= 1 && info.channels <= 2 &&
            info.hz > 0) {
            const unsigned long deadline = millis() + 2000;
            while (M5Cardputer.Speaker.isPlaying(0) >= 2 &&
                   millis() < deadline) {
                if (pollAudioCancel()) {
                    error = "SPEECH CANCELLED";
                    return false;
                }
                delay(1);
            }
            if (M5Cardputer.Speaker.isPlaying(0) >= 2) {
                error = "MP3 SPEAKER TIMEOUT";
                return false;
            }
            if (!M5Cardputer.Speaker.playRaw(
                    pcm[pcmIndex], samples * info.channels, info.hz,
                    info.channels == 2, 1, 0, false)) {
                error = "MP3 SPEAKER QUEUE FAILED";
                return false;
            }
            pcmIndex ^= 1;
            decodedAny = true;
        }
        yield();
    }
    if (!decodedAny) error = "INVALID MP3 AUDIO";
    return decodedAny;
}

bool HermesAudioClient::playWav(File& file, String& error)
{
    char riff[4] = {};
    char wave[4] = {};
    file.read(reinterpret_cast<std::uint8_t*>(riff), sizeof(riff));
    read32(file);
    file.read(reinterpret_cast<std::uint8_t*>(wave), sizeof(wave));
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t rate = 0;
    std::uint32_t dataBytes = 0;
    while (file.available()) {
        char chunk[4] = {};
        if (file.read(reinterpret_cast<std::uint8_t*>(chunk), sizeof(chunk)) !=
            sizeof(chunk)) break;
        const std::uint32_t size = read32(file);
        const std::size_t start = file.position();
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            format = read16(file);
            channels = read16(file);
            rate = read32(file);
            read32(file);
            read16(file);
            bits = read16(file);
        } else if (memcmp(chunk, "data", 4) == 0) {
            dataBytes = min<std::uint32_t>(size, file.size() - file.position());
            break;
        }
        file.seek(start + size + (size & 1));
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0 ||
        format != 1 || (channels != 1 && channels != 2) || bits != 16 ||
        !rate || !dataBytes) {
        error = "UNSUPPORTED WAV AUDIO";
        return false;
    }
    static std::int16_t pcm[2][2048];
    std::uint8_t pcmIndex = 0;
    while (dataBytes) {
        if (pollAudioCancel()) {
            error = "SPEECH CANCELLED";
            return false;
        }
        std::size_t bytes = min<std::size_t>(dataBytes, sizeof(pcm[0]));
        bytes -= bytes % (channels * sizeof(std::int16_t));
        if (!bytes || file.read(reinterpret_cast<std::uint8_t*>(pcm[pcmIndex]),
                                bytes) != bytes) {
            error = "WAV AUDIO TRUNCATED";
            return false;
        }
        const unsigned long deadline = millis() + 2000;
        while (M5Cardputer.Speaker.isPlaying(0) >= 2 &&
               millis() < deadline) {
            if (pollAudioCancel()) {
                error = "SPEECH CANCELLED";
                return false;
            }
            delay(1);
        }
        if (M5Cardputer.Speaker.isPlaying(0) >= 2) {
            error = "WAV SPEAKER TIMEOUT";
            return false;
        }
        if (!M5Cardputer.Speaker.playRaw(pcm[pcmIndex], bytes / 2, rate,
                                         channels == 2, 1, 0, false)) {
            error = "WAV SPEAKER QUEUE FAILED";
            return false;
        }
        dataBytes -= bytes;
        pcmIndex ^= 1;
        yield();
    }
    return true;
}

}  // namespace hermes_terminal
