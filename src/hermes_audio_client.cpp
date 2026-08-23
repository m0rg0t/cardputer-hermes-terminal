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
constexpr std::uint8_t kEs8311Address = 0x18;
constexpr std::uint8_t kDacPowerRegister = 0x12;
constexpr std::uint8_t kDacMuteRegister = 0x31;
constexpr std::uint8_t kDacVolumeRegister = 0x32;
constexpr std::uint8_t kHeadphoneRegister = 0x13;
constexpr std::uint8_t kI2sBitClockPin = 41;
constexpr std::uint8_t kI2sDataOutPin = 42;
constexpr std::uint8_t kI2sWordSelectPin = 43;

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

String decodeChunked(const String& raw)
{
    String output;
    int offset = 0;
    while (offset < static_cast<int>(raw.length())) {
        const int end = raw.indexOf("\r\n", offset);
        if (end < 0) return "";
        const std::size_t size = strtoul(raw.substring(offset, end).c_str(),
                                        nullptr, 16);
        if (size == 0) return output;
        offset = end + 2;
        if (offset + static_cast<int>(size) + 2 >
            static_cast<int>(raw.length())) return "";
        output += raw.substring(offset, offset + size);
        offset += size + 2;
    }
    return "";
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
        if (logical < prefix_.length()) return prefix_[logical];
        const std::size_t encodedEnd = totalSize_ - suffix_.length();
        if (logical >= encodedEnd) return suffix_[logical - encodedEnd];
        if (encodedOffset_ >= encodedLength_) {
            std::uint8_t input[3] = {};
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
    std::uint8_t encoded_[4] = {};
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
        while (!client_.available()) {
            if ((!client_.connected() && !client_.available()) ||
                millis() - started >= kAudioHttpTimeoutMs) return -1;
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
};

bool readHttpHeaders(WiFiClientSecure& client, int& status, bool& chunked,
                     int& contentLength)
{
    String headers;
    const unsigned long started = millis();
    while (headers.indexOf("\r\n\r\n") < 0) {
        if (client.available()) {
            headers += static_cast<char>(client.read());
            if (headers.length() > 4096) return false;
        } else if ((!client.connected() && !client.available()) ||
                   millis() - started >= kAudioHttpTimeoutMs) {
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
    if (!client.connect(host.c_str(), port)) {
        error = "TRANSCRIBE TLS FAILED";
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
    while (body.available() > 0) {
        std::size_t count = 0;
        while (count < sizeof(buffer) && body.available() > 0) {
            const int value = body.read();
            if (value < 0) break;
            buffer[count++] = value;
        }
        std::size_t sent = 0;
        while (sent < count) {
            const std::size_t written = client.write(buffer + sent,
                                                     count - sent);
            if (!written) break;
            sent += written;
            yield();
        }
        if (!count || sent != count) {
            error = "TRANSCRIBE UPLOAD FAILED";
            client.stop();
            return false;
        }
        yield();
    }

    String headers;
    unsigned long lastDataMs = millis();
    while (headers.indexOf("\r\n\r\n") < 0 &&
           millis() - lastDataMs < kAudioHttpTimeoutMs) {
        while (client.available()) {
            headers += static_cast<char>(client.read());
            lastDataMs = millis();
            if (headers.length() > 4096) break;
        }
        if (!client.connected() && !client.available()) break;
        delay(2);
    }
    const int headerEnd = headers.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        error = "TRANSCRIBE TIMEOUT";
        client.stop();
        return false;
    }
    const int statusSpace = headers.indexOf(' ');
    const int status = statusSpace >= 0
                           ? headers.substring(statusSpace + 1,
                                               statusSpace + 4).toInt()
                           : 0;
    String lowerHeaders = headers.substring(0, headerEnd);
    lowerHeaders.toLowerCase();
    const bool chunked = lowerHeaders.indexOf("transfer-encoding: chunked") >= 0;
    int expected = -1;
    const int lengthAt = lowerHeaders.indexOf("content-length:");
    if (lengthAt >= 0) {
        expected = lowerHeaders.substring(lengthAt + 15).toInt();
    }
    String response = headers.substring(headerEnd + 4);
    lastDataMs = millis();
    while (response.length() < 8192 &&
           (expected < 0 || static_cast<int>(response.length()) < expected) &&
           millis() - lastDataMs < kAudioHttpTimeoutMs) {
        while (client.available() && response.length() < 8192) {
            response += static_cast<char>(client.read());
            lastDataMs = millis();
        }
        if (!client.connected() && !client.available()) break;
        delay(2);
    }
    client.stop();
    if (chunked) response = decodeChunked(response);
    if (status != 200) {
        error = status == 401 ? "AUTH EXPIRED (TRANSCRIBE 401)"
                              : status == 403 ? "AUTH FORBIDDEN (TRANSCRIBE 403)"
                                              : "TRANSCRIBE HTTP " + String(status);
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
    if (!client.connect(host.c_str(), port)) {
        error = "TTS TLS FAILED";
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
    if (client.write(reinterpret_cast<const std::uint8_t*>(requestBody.c_str()),
                     requestBody.length()) != requestBody.length()) {
        error = "TTS UPLOAD FAILED";
        client.stop();
        return false;
    }

    int status = 0;
    int contentLength = -1;
    bool chunked = false;
    if (!readHttpHeaders(client, status, chunked, contentLength)) {
        error = "TTS TIMEOUT";
        client.stop();
        return false;
    }
    if (status != 200) {
        error = status == 401 ? "AUTH EXPIRED (TTS 401)"
                              : status == 403 ? "AUTH FORBIDDEN (TTS 403)"
                                              : "TTS HTTP " + String(status);
        client.stop();
        return false;
    }
    HttpBodyReader body(client, chunked, contentLength);
    const char token[] = "\"data_url\"";
    std::size_t matched = 0;
    int value = -1;
    while ((value = body.read()) >= 0 && matched < sizeof(token) - 1) {
        matched = value == token[matched] ? matched + 1
                                          : (value == token[0] ? 1 : 0);
    }
    if (matched != sizeof(token) - 1) {
        error = "TTS DATA MISSING";
        client.stop();
        return false;
    }
    while ((value = body.read()) >= 0 && value != ':') {}
    while ((value = body.read()) >= 0 && value != '"') {}
    if (value < 0) {
        error = "TTS INVALID JSON";
        client.stop();
        return false;
    }
    String prefix;
    while ((value = body.read()) >= 0 && value != ',' && value != '"') {
        if (prefix.length() < 96) prefix += static_cast<char>(value);
    }
    if (value != ',' || !prefix.startsWith("data:") ||
        prefix.indexOf(";base64") < 0) {
        error = "TTS INVALID DATA URL";
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
    char quartet[4] = {};
    std::size_t quartetLength = 0;
    std::size_t totalWritten = 0;
    bool closed = false;
    while ((value = body.read()) >= 0) {
        if (value == '"') {
            closed = true;
            break;
        }
        if (value == '\r' || value == '\n' || value == ' ') continue;
        quartet[quartetLength++] = static_cast<char>(value);
        if (quartetLength == sizeof(quartet)) {
            std::uint8_t decoded[3] = {};
            std::size_t decodedLength = 0;
            if (mbedtls_base64_decode(decoded, sizeof(decoded), &decodedLength,
                                      reinterpret_cast<std::uint8_t*>(quartet),
                                      sizeof(quartet)) != 0 ||
                output.write(decoded, decodedLength) != decodedLength) {
                error = "TTS DECODE FAILED";
                output.close();
                client.stop();
                return false;
            }
            totalWritten += decodedLength;
            quartetLength = 0;
        }
        if ((totalWritten & 0x1FFF) == 0) yield();
    }
    output.flush();
    output.close();
    client.stop();
    if (!closed || quartetLength != 0 || totalWritten < 16) {
        error = "TTS AUDIO TRUNCATED";
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

void HermesAudioClient::endSpeaker()
{
    const unsigned long deadline = millis() + 3000;
    while (M5Cardputer.Speaker.isPlaying() && millis() < deadline) delay(2);
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
}

bool HermesAudioClient::testSpeaker(String& error)
{
    if (!uiCuesEnabled_) {
        error = "AUDIO ALERTS OFF";
        return false;
    }
    if (!beginSpeaker(error)) return false;
    const bool started = M5Cardputer.Speaker.tone(880.0f, 100);
    if (!started) error = "SPEAKER TEST FAILED";
    endSpeaker();
    return started;
}

bool HermesAudioClient::playUiCue(UiCue cue, String& error)
{
    error = "";
    if (!uiCuesEnabled_) return true;
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
    endSpeaker();
    return started;
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
    endSpeaker();
    file.close();
    return result;
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
                   millis() < deadline) delay(1);
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
        std::size_t bytes = min<std::size_t>(dataBytes, sizeof(pcm[0]));
        bytes -= bytes % (channels * sizeof(std::int16_t));
        if (!bytes || file.read(reinterpret_cast<std::uint8_t*>(pcm[pcmIndex]),
                                bytes) != bytes) {
            error = "WAV AUDIO TRUNCATED";
            return false;
        }
        const unsigned long deadline = millis() + 2000;
        while (M5Cardputer.Speaker.isPlaying(0) >= 2 &&
               millis() < deadline) delay(1);
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
