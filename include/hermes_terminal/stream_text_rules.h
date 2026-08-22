#pragma once

#include <cstddef>
#include <cstdint>

namespace hermes_terminal {

enum class CompletionAppendKind : std::uint8_t {
    kNone,
    kSuffix,
    kFullWithLabel,
};

struct CompletionAppendDecision {
    constexpr CompletionAppendDecision(
        CompletionAppendKind selected = CompletionAppendKind::kNone,
        std::size_t appendOffset = 0)
        : kind(selected), offset(appendOffset)
    {
    }

    CompletionAppendKind kind;
    std::size_t offset;
};

inline bool textEquals(const char* left, std::size_t leftLength,
                       const char* right, std::size_t rightLength)
{
    if (leftLength != rightLength) return false;
    for (std::size_t index = 0; index < leftLength; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

inline bool textStartsWith(const char* text, std::size_t textLength,
                           const char* prefix, std::size_t prefixLength)
{
    return textLength >= prefixLength &&
           textEquals(text, prefixLength, prefix, prefixLength);
}

inline CompletionAppendDecision decideCompletionAppend(
    const char* streamed, std::size_t streamedLength,
    const char* interim, std::size_t interimLength,
    const char* complete, std::size_t completeLength)
{
    if (!completeLength) return {};
    if (streamedLength &&
        textStartsWith(complete, completeLength, streamed, streamedLength)) {
        return {CompletionAppendKind::kSuffix, streamedLength};
    }
    if (!streamedLength && interimLength &&
        textStartsWith(complete, completeLength, interim, interimLength)) {
        return {CompletionAppendKind::kSuffix, interimLength};
    }
    if ((!streamedLength ||
         !textEquals(complete, completeLength, streamed, streamedLength)) &&
        !textEquals(complete, completeLength, interim, interimLength)) {
        return {CompletionAppendKind::kFullWithLabel, 0};
    }
    return {};
}

}  // namespace hermes_terminal
