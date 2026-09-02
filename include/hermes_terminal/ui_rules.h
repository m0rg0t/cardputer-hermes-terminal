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
// Rows are byte buffers: Cyrillic code points are two bytes but one column.
constexpr std::size_t kWrapMaxColumns = 48;
constexpr std::size_t kWrapRowBytes = kWrapMaxColumns * 2 + 1;

// Decodes one UTF-8 sequence. Returns the byte length consumed (at least 1)
// and writes the code point; malformed bytes decode as U+FFFD.
inline std::size_t decodeUtf8(const char* text, std::size_t length,
                              std::uint32_t& codePoint)
{
    const unsigned char lead = static_cast<unsigned char>(text[0]);
    std::size_t need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    if (lead < 0x80) { codePoint = lead; return 1; }
    if (need == 0 || need >= length) { codePoint = 0xFFFD; return 1; }
    codePoint = lead & (0x3F >> need);
    for (std::size_t k = 1; k <= need; ++k) {
        const unsigned char c = static_cast<unsigned char>(text[k]);
        if ((c & 0xC0) != 0x80) { codePoint = 0xFFFD; return 1; }
        codePoint = (codePoint << 6) | (c & 0x3F);
    }
    return need + 1;
}

// The mini renderer covers U+0400..U+04FF; anything else non-ASCII folds
// to '?'. Callers pass keepCyrillic=false when the glyph set is compiled
// out, so the fold covers everything non-ASCII.
inline bool isRenderableCyrillic(std::uint32_t codePoint)
{
    return codePoint >= 0x0400 && codePoint <= 0x04FF;
}

// ASCII stand-in for typographic punctuation the fonts do not carry, or 0.
inline char asciiFallback(std::uint32_t codePoint)
{
    switch (codePoint) {
        case 0x00A0: return ' ';            // no-break space
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
        case 0x2015: case 0x2212: return '-';
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x00AB: case 0x00BB: case 0x201C: case 0x201D: case 0x201E:
        case 0x2033: return '"';
        case 0x2022: case 0x00B7: return '*';
        case 0x2026: return '.';            // ellipsis: one dot per column
        case 0x2192: return '>';
        case 0x2190: return '<';
        case 0x00D7: return 'x';
        default: return 0;
    }
}

// Number of text columns (6 px cells) a UTF-8 string occupies.
inline std::size_t utf8Columns(const char* text, std::size_t length)
{
    std::size_t columns = 0;
    for (std::size_t i = 0; i < length;) {
        std::uint32_t cp;
        i += decodeUtf8(text + i, length - i, cp);
        ++columns;
    }
    return columns;
}

// Byte length of the longest prefix that fits in maxColumns columns.
inline std::size_t utf8PrefixBytes(const char* text, std::size_t length,
                                   std::size_t maxColumns)
{
    std::size_t columns = 0;
    std::size_t i = 0;
    while (i < length && columns < maxColumns) {
        std::uint32_t cp;
        i += decodeUtf8(text + i, length - i, cp);
        ++columns;
    }
    return i;
}

// Replaces every multibyte UTF-8 code point in a NUL-terminated buffer with
// '?', in place. Returns the new length.
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

// Greedy monospace word wrap over UTF-8. Emits each row as (bytes, length).
// Explicit '\n' always breaks; words longer than a row are hard split;
// a trailing space is dropped. Cyrillic code points are kept (one column
// each) when keepCyrillic is set; other multibyte code points fold to '?'.
template <typename Emit>
inline void wrapMonospace(const char* text, std::size_t length,
                          std::size_t cols, Emit emit, bool keepCyrillic = true)
{
    if (cols == 0 || cols > kWrapMaxColumns) cols = kWrapMaxColumns;
    char row[kWrapRowBytes];
    std::size_t rowBytes = 0;
    std::size_t rowCols = 0;
    std::size_t lastSpace = static_cast<std::size_t>(-1);  // byte index
    bool pendingRow = false;
    auto flush = [&](std::size_t upTo) {
        emit(row, upTo);
        pendingRow = false;
    };
    auto columnsOf = [](const char* data, std::size_t bytes) {
        return utf8Columns(data, bytes);
    };
    for (std::size_t index = 0; index < length;) {
        std::uint32_t cp;
        const std::size_t consumed = decodeUtf8(text + index, length - index, cp);
        const char* bytes = text + index;
        std::size_t count = consumed;
        char folded = 0;
        index += consumed;
        if (cp >= 0x80 && !(keepCyrillic && isRenderableCyrillic(cp))) {
            const char fallback = asciiFallback(cp);
            folded = fallback ? fallback : '?';
            bytes = &folded;
            count = 1;
        }
        const char glyph = count == 1 ? bytes[0] : '\x01';
        if (glyph == '\r') continue;
        if (glyph == '\n') {
            flush(rowBytes);
            rowBytes = rowCols = 0;
            lastSpace = static_cast<std::size_t>(-1);
            continue;
        }
        if (glyph == ' ' && rowCols == 0) continue;
        if (rowCols == cols) {
            if (lastSpace != static_cast<std::size_t>(-1)) {
                flush(lastSpace);
                const std::size_t carry = rowBytes - lastSpace - 1;
                for (std::size_t k = 0; k < carry; ++k) row[k] = row[lastSpace + 1 + k];
                rowBytes = carry;
                rowCols = columnsOf(row, rowBytes);
            } else {
                flush(rowBytes);
                rowBytes = rowCols = 0;
            }
            lastSpace = static_cast<std::size_t>(-1);
            if (glyph == ' ' && rowCols == 0) continue;
        }
        if (glyph == ' ') lastSpace = rowBytes;
        for (std::size_t k = 0; k < count; ++k) row[rowBytes++] = bytes[k];
        ++rowCols;
        pendingRow = true;
    }
    if (pendingRow) {
        while (rowBytes && row[rowBytes - 1] == ' ') --rowBytes;
        flush(rowBytes);
    }
}

}  // namespace hermes_terminal
