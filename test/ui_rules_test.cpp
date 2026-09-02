#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

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
    // Monospace word wrap: breaks on spaces, keeps explicit newlines, hard
    // splits words longer than a row, keeps Cyrillic as one column per code
    // point, and folds other multibyte code points to '?'.
    auto wrap = [](const char* text, std::size_t cols, bool keep = true) {
        std::vector<std::string> rows;
        wrapMonospace(text, std::strlen(text), cols,
                      [&](const char* line, std::size_t length) {
                          rows.emplace_back(line, length);
                      }, keep);
        return rows;
    };
    auto rows = wrap("Add certificate expiry to the pre-deploy checklist.", 20);
    assert(rows.size() == 4);
    assert(rows[0] == "Add certificate");
    assert(rows[1] == "expiry to the");
    assert(rows[2] == "pre-deploy");
    assert(rows[3] == "checklist.");
    rows = wrap("a\n\nb", 10);
    assert(rows.size() == 3 && rows[0] == "a" && rows[1] == "" && rows[2] == "b");
    rows = wrap("abcdefghijkl", 5);
    assert(rows.size() == 3 && rows[0] == "abcde" && rows[2] == "kl");
    rows = wrap("\xD0\x9F\xD1\x80\xD0\xB8 ok", 10);
    assert(rows.size() == 1 && rows[0] == "\xD0\x9F\xD1\x80\xD0\xB8 ok");
    rows = wrap("\xD0\x9F\xD1\x80\xD0\xB8 ok", 10, false);
    assert(rows.size() == 1 && rows[0] == "??? ok");
    // Cyrillic words wrap by columns, not bytes: 4 letters + space + 3.
    rows = wrap("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80", 6);
    assert(rows.size() == 2 && rows[1] == "\xD0\xBC\xD0\xB8\xD1\x80");
    assert(utf8Columns(rows[0].c_str(), rows[0].size()) == 6);
    rows = wrap("caf\xC3\xA9 \xE2\x9C\x93", 10);
    assert(rows.size() == 1 && rows[0] == "caf? ?");
    // Typographic punctuation folds to ASCII stand-ins instead of '?'.
    rows = wrap("a \xE2\x80\x94 \xE2\x80\x9C" "b" "\xE2\x80\x9D", 20);
    assert(rows.size() == 1 && rows[0] == "a - \"b\"");
    assert(asciiFallback(0x2014) == '-' && asciiFallback(0x2713) == 0);
    rows = wrap("", 10);
    assert(rows.empty());
    rows = wrap("trailing space ", 20);
    assert(rows.size() == 1 && rows[0] == "trailing space");
    // Column counting and prefix truncation work on code points.
    assert(utf8Columns("abc", 3) == 3);
    assert(utf8Columns("\xD0\x94\xD0\xB0", 4) == 2);
    assert(utf8PrefixBytes("\xD0\x94\xD0\xB0\xD1\x87\xD0\xB0", 8, 2) == 4);
    assert(utf8PrefixBytes("abc", 3, 10) == 3);
    std::uint32_t cp = 0;
    assert(decodeUtf8("\xD0\x96", 2, cp) == 2 && cp == 0x0416);
    assert(decodeUtf8("\xE2\x9C\x93", 3, cp) == 3 && cp == 0x2713);
    assert(decodeUtf8("\xD0", 1, cp) == 1 && cp == 0xFFFD);
    // Folding alone, for single-line labels.
    char label[] = "T\xC3\xA9st";
    assert(foldUtf8ToAscii(label) == 4);
    assert(std::strcmp(label, "T?st") == 0);
    return 0;
}
