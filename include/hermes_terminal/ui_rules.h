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

}  // namespace hermes_terminal
