#pragma once

#include <cstddef>
#include <cstdint>

// Small pure rules shared by the application layer and the native tests.

namespace hermes_terminal {

// Sentinel used by the app to mean "follow the newest transcript line".
constexpr int kScrollFollowBottom = 32767;
// Upper bounds for paged REST imports (records), so a gateway that ignores
// the offset parameter cannot loop forever and fill the SD card.
constexpr std::size_t kMaxSessionsImport = 5000;
constexpr std::size_t kMaxHistoryImport = 20000;

inline int clampTimelineScroll(int scroll, int maxScroll)
{
    if (maxScroll < 0) maxScroll = 0;
    if (scroll > maxScroll) return maxScroll;
    return scroll < 0 ? 0 : scroll;
}

inline bool pagedImportContinues(std::size_t records, std::size_t pageSize,
                                 std::size_t offset, std::size_t cap)
{
    return records == pageSize && offset + records < cap;
}

inline std::size_t utf8BomLength(const char* data, std::size_t length)
{
    if (length < 3) return 0;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    return bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
}

// Largest row the wrap helper emits (240 px / 6 px glyphs = 40 columns).
constexpr std::size_t kWrapMaxColumns = 48;

// Replaces every multibyte UTF-8 code point in a NUL-terminated buffer with
// '?', in place. Returns the new length. Font 1 has ASCII glyphs only, so
// without this a Cyrillic title renders as an empty row.
inline std::size_t foldUtf8ToAscii(char* text)
{
    std::size_t out = 0;
    for (std::size_t in = 0; text[in]; ++in) {
        const unsigned char c = static_cast<unsigned char>(text[in]);
        if (c < 0x80) text[out++] = static_cast<char>(c);
        else if ((c & 0xC0) != 0x80) text[out++] = '?';
    }
    text[out] = '\0';
    return out;
}

// Greedy monospace word wrap. Emits each row as (const char*, length).
// Explicit '\n' always breaks; words longer than a row are hard split;
// multibyte code points fold to '?'. A trailing space is dropped.
template <typename Emit>
inline void wrapMonospace(const char* text, std::size_t length,
                          std::size_t cols, Emit emit)
{
    if (cols == 0 || cols > kWrapMaxColumns) cols = kWrapMaxColumns;
    char row[kWrapMaxColumns + 1];
    std::size_t rowLength = 0;
    std::size_t lastSpace = static_cast<std::size_t>(-1);
    bool pendingRow = false;
    auto flush = [&](std::size_t upTo) {
        emit(row, upTo);
        pendingRow = false;
    };
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char c = static_cast<unsigned char>(text[index]);
        if ((c & 0xC0) == 0x80) continue;
        const char glyph = c < 0x80 ? static_cast<char>(c) : '?';
        if (glyph == '\r') continue;
        if (glyph == '\n') {
            flush(rowLength);
            rowLength = 0;
            lastSpace = static_cast<std::size_t>(-1);
            continue;
        }
        if (glyph == ' ' && rowLength == 0) continue;
        if (rowLength == cols) {
            if (lastSpace != static_cast<std::size_t>(-1)) {
                // Wrap at the last space; carry the partial word over.
                flush(lastSpace);
                const std::size_t carry = rowLength - lastSpace - 1;
                for (std::size_t k = 0; k < carry; ++k) row[k] = row[lastSpace + 1 + k];
                rowLength = carry;
            } else {
                flush(rowLength);
                rowLength = 0;
            }
            lastSpace = static_cast<std::size_t>(-1);
            if (glyph == ' ' && rowLength == 0) continue;
        }
        if (glyph == ' ') lastSpace = rowLength;
        row[rowLength++] = glyph;
        pendingRow = true;
    }
    if (pendingRow) {
        while (rowLength && row[rowLength - 1] == ' ') --rowLength;
        flush(rowLength);
    }
}

}  // namespace hermes_terminal
