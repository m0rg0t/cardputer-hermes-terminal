#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hermes_terminal/audio_rules.h"

using namespace hermes_terminal;

struct Body {
    std::string text;
    std::size_t offset = 0;
    int read() { return offset < text.size() ? text[offset++] : -1; }
};

struct File {
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
    bool failSeek = false;
    std::size_t position() const { return offset; }
    std::size_t size() const { return bytes.size(); }
    std::size_t read(std::uint8_t* out, std::size_t count) {
        if (count > size() - offset) count = size() - offset;
        if (count) std::memcpy(out, bytes.data() + offset, count);
        offset += count;
        return count;
    }
    bool seek(std::size_t target) {
        if (failSeek || target > size()) return false;
        offset = target;
        return true;
    }
};

void append32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) bytes.push_back(value >> shift);
}

File wav(bool stereo = false, bool oddChunk = false)
{
    File file;
    auto& b = file.bytes;
    b.insert(b.end(), {'R','I','F','F'});
    append32(b, oddChunk ? 50 : 40);
    b.insert(b.end(), {'W','A','V','E'});
    if (oddChunk) {
        b.insert(b.end(), {'J','U','N','K'});
        append32(b, 1);
        b.insert(b.end(), {0,0}); // payload + padding
    }
    b.insert(b.end(), {'f','m','t',' '});
    append32(b, 16);
    b.insert(b.end(), {1,0,static_cast<std::uint8_t>(stereo ? 2 : 1),0});
    append32(b, 16000);
    append32(b, stereo ? 64000 : 32000);
    b.insert(b.end(), {static_cast<std::uint8_t>(stereo ? 4 : 2),0,16,0});
    b.insert(b.end(), {'d','a','t','a'});
    append32(b, 4);
    b.insert(b.end(), {0,0,0,0});
    return file;
}

int main()
{
    // The deployed FastAPI response is compact. Finding the key must leave
    // ':' available, otherwise the data URL's own colon gets mistaken for it.
    for (const char* json : {
            "{\"ok\":true,\"data_url\":\"data:audio/mpeg;base64,AQID\"}",
            "{\"data_url\" : \"data:audio/wav;base64,AQID\"}",
            "{\"data_url\""}) {
        Body body{json};
        assert(findAudioDataUrl(body));
        const int next = body.read();
        assert(next == ':' || next == ' ' || next == -1);
    }
    Body missing{"{\"ok\":false}"};
    assert(!findAudioDataUrl(missing));
    Body truncated{"{\"data_ur"};
    assert(!findAudioDataUrl(truncated));

    PcmWavHeader header;
    for (bool stereo : {false, true}) {
        for (bool oddChunk : {false, true}) {
            File file = wav(stereo, oddChunk);
            assert(readPcmWavHeader(file, header));
            assert(header.channels == (stereo ? 2 : 1));
            assert(header.rate == 16000 && header.dataBytes == 4);
            assert(file.position() == file.size() - 4);
        }
    }
    // Every truncation, including a partial chunk header, is rejected.
    const File valid = wav();
    for (std::size_t length = 0; length < valid.size(); ++length) {
        File file = valid;
        file.bytes.resize(length);
        assert(!readPcmWavHeader(file, header));
    }
    File huge = wav(false, true);
    for (std::size_t i = 16; i < 20; ++i) huge.bytes[i] = 0xff;
    assert(!readPcmWavHeader(huge, header));
    File badSeek = wav();
    badSeek.failSeek = true;
    assert(!readPcmWavHeader(badSeek, header));
    File unsupported = wav();
    unsupported.bytes[20] = 3; // IEEE float
    assert(!readPcmWavHeader(unsupported, header));
    File misaligned = wav(true);
    misaligned.bytes[40] = 3;
    assert(!readPcmWavHeader(misaligned, header));
    File missingPadding = wav(false, true);
    missingPadding.bytes.resize(21);
    assert(!readPcmWavHeader(missingPadding, header));
    File dataFirst = wav();
    std::memcpy(dataFirst.bytes.data() + 12, "data", 4);
    assert(!readPcmWavHeader(dataFirst, header));
}
