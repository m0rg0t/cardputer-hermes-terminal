#include <cassert>
#include <cstddef>

#include "hermes_terminal/ui_rules.h"

using namespace hermes_terminal;

int main()
{
    // The "follow the bottom" sentinel collapses to the real maximum so the
    // next scroll-up key press moves immediately.
    assert(clampTimelineScroll(32767, 40) == 40);
    assert(clampTimelineScroll(12, 40) == 12);
    assert(clampTimelineScroll(-5, 40) == 0);
    assert(clampTimelineScroll(32767, 0) == 0);
    // Paged REST imports stop when a page is short or the cap is reached, so
    // a server that ignores offset cannot fill the card forever.
    assert(pagedImportContinues(100, 100, 0, kMaxSessionsImport));
    assert(!pagedImportContinues(99, 100, 0, kMaxSessionsImport));
    assert(!pagedImportContinues(100, 100, kMaxSessionsImport - 100,
                                 kMaxSessionsImport));
    assert(pagedImportContinues(48, 48, 48, kMaxHistoryImport));
    assert(!pagedImportContinues(48, 48, kMaxHistoryImport, kMaxHistoryImport));
    // A UTF-8 byte order mark at the head of HERMES.CFG is skipped.
    const char bom[] = "\xEF\xBB\xBFwifi_ssid=x";
    assert(utf8BomLength(bom, sizeof(bom) - 1) == 3);
    assert(utf8BomLength("wifi_ssid=x", 11) == 0);
    assert(utf8BomLength("\xEF\xBB", 2) == 0);
    return 0;
}
