#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hermes_terminal/audio_upload.h"

#ifdef HERMES_TEST_MBEDTLS
#include <mbedtls/base64.h>
#endif

using namespace hermes_terminal;

struct AudioFile {
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
    std::size_t maxRead = 192;
    std::size_t size() const { return bytes.size(); }
    std::size_t read(std::uint8_t* out, std::size_t count) {
        if (count > maxRead) count = maxRead;
        if (count > size() - offset) count = size() - offset;
        if (count) std::memcpy(out, bytes.data() + offset, count);
        offset += count;
        return count;
    }
};

std::string zeroBase64(std::size_t bytes)
{
    std::string output((bytes / 3) * 4, 'A');
    if (bytes % 3 == 1) output += "AA==";
    if (bytes % 3 == 2) output += "AAA=";
    return output;
}

// A portable fixture with the mbedTLS capacity contract. Also run this same
// suite against real mbedTLS with -DHERMES_TEST_MBEDTLS and -lmbedcrypto.
int encodeZeros(unsigned char* out, std::size_t capacity, std::size_t* written,
                const unsigned char* in, std::size_t count)
{
    const std::string encoded = zeroBase64(count);
    if (capacity < encoded.size() + 1) {
        *written = encoded.size() + 1;
        return -1;
    }
    for (std::size_t i = 0; i < count; ++i) assert(in[i] == 0);
    std::memcpy(out, encoded.c_str(), encoded.size() + 1);
    *written = encoded.size();
    return 0;
}

int main()
{
#ifdef HERMES_TEST_MBEDTLS
    const auto encode = mbedtls_base64_encode;
#else
    const auto encode = encodeZeros;
#endif
    // Full chunks require 257 bytes. Test both padding cases and the 30 s
    // recording size; short SD reads must not introduce mid-stream padding.
    for (std::size_t size : {0U,1U,2U,3U,44U,190U,191U,192U,193U,194U,
                             384U,385U,960044U}) {
        for (std::size_t maxRead : {7U,192U}) {
            AudioFile file{std::vector<std::uint8_t>(size), 0, maxRead};
            AudioJsonStream<AudioFile> body(file, encode);
            const std::string expected = "{\"data_url\":\"data:audio/wav;base64," +
                zeroBase64(size) + "\",\"mime_type\":\"audio/wav\"}";
            assert(body.totalSize() == expected.size());
            std::string actual;
            while (body.available()) {
                const auto remaining = body.available();
                const int value = body.peek();
                assert(value >= 0);
                assert(body.peek() == value && body.available() == remaining);
                assert(body.read() == value);
                actual += static_cast<char>(value);
            }
            assert(actual == expected);
            assert(body.read() == -1 && body.peek() == -1);
        }
    }
    AudioFile unreadable{std::vector<std::uint8_t>(192), 0, 0};
    AudioJsonStream<AudioFile> body(unreadable, encode);
    while (body.read() >= 0) {}
    assert(body.available() > 0); // Upload fails instead of sending a short JSON body.
}
