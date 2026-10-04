#pragma once

#include <cstddef>
#include <cstdint>

namespace hermes_terminal {

constexpr char kAudioUploadPrefix[] = "{\"data_url\":\"data:audio/wav;base64,";
constexpr char kAudioUploadSuffix[] = "\",\"mime_type\":\"audio/wav\"}";

// Stream the Desktop WAV JSON contract without duplicating the clip in RAM.
// The encoder follows mbedTLS' API: capacity includes a terminating NUL, while
// the successful output length excludes it.
template <typename File>
class AudioJsonStream {
public:
    using Encoder = int (*)(unsigned char*, std::size_t, std::size_t*,
                            const unsigned char*, std::size_t);

    AudioJsonStream(File file, Encoder encoder) : file_(file), encoder_(encoder)
    {
        audioSize_ = file_.size();
        totalSize_ = sizeof(kAudioUploadPrefix) - 1 + ((audioSize_ + 2) / 3) * 4 +
                     sizeof(kAudioUploadSuffix) - 1;
    }

    std::size_t available() const { return totalSize_ - position_; }
    std::size_t totalSize() const { return totalSize_; }
    int read()
    {
        const int value = peeked_ >= 0 ? peeked_ : next();
        peeked_ = -1;
        if (value >= 0) ++position_;
        return value;
    }
    int peek()
    {
        if (peeked_ < 0) peeked_ = next();
        return peeked_;
    }

private:
    int next()
    {
        if (position_ >= totalSize_) return -1;
        if (position_ < sizeof(kAudioUploadPrefix) - 1) return kAudioUploadPrefix[position_];
        const std::size_t encodedEnd = totalSize_ - (sizeof(kAudioUploadSuffix) - 1);
        if (position_ >= encodedEnd) return kAudioUploadSuffix[position_ - encodedEnd];
        if (encodedOffset_ >= encodedLength_) {
            std::uint8_t input[192];
            const std::size_t remaining = audioSize_ - audioRead_;
            const std::size_t wanted = remaining < sizeof(input)
                                          ? remaining : sizeof(input);
            if (!wanted) return -1;
            std::size_t count = 0;
            while (count < wanted) {
                const std::size_t read = file_.read(input + count, wanted - count);
                if (!read) return -1;
                count += read;
            }
            audioRead_ += count;
            std::size_t written = 0;
            if (encoder_(encoded_, sizeof(encoded_), &written, input, count))
                return -1;
            encodedOffset_ = 0;
            encodedLength_ = written;
        }
        return encoded_[encodedOffset_++];
    }

    File file_;
    Encoder encoder_;
    std::size_t audioSize_ = 0;
    std::size_t audioRead_ = 0;
    std::size_t totalSize_ = 0;
    std::size_t position_ = 0;
    // 192 input bytes encode to 256 characters plus the encoder's NUL.
    std::uint8_t encoded_[257] = {};
    std::size_t encodedOffset_ = 0;
    std::size_t encodedLength_ = 0;
    int peeked_ = -1;
};

}  // namespace hermes_terminal
