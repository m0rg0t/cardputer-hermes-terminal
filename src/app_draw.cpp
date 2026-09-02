#include "hermes_terminal/app.h"

#include <WiFi.h>

#ifndef HERMES_SLEEP_STATES
#define HERMES_SLEEP_STATES 1
#endif
#if HERMES_SLEEP_STATES
#include "hermes_terminal/hermes_states.h"
#endif
#include "hermes_terminal/hermes_badge.h"
#include "hermes_terminal/ui_rules.h"
#include "app_ui.h"

namespace hermes_terminal {
namespace {

template <typename Surface>
void drawBackgroundAccents(Surface& display)
{
    // Static calibration rail: visible enough to add hardware character, but
    // kept outside the 8 px text cells and session rows.
    display.drawFastHLine(4, 28, 232, kUiRule);
    display.fillRect(220, 27, 6, 2, kUiRed);
    display.drawPixel(230, 27, kUiMuted);
    display.drawPixel(234, 27, kUiMuted);
}

template <typename Surface>
void drawPocketHeader(Surface& display, const char* section, bool connected)
{
    display.setTextWrap(false);
    display.setTextFont(1);
    display.setTextColor(kUiInk, kUiBg);
    display.setCursor(4, 3);
    display.printf("HERMES // %s", section);
    display.setTextColor(kUiMuted, kUiBg);
    display.setCursor(199, 3);
    display.print("LINK");
    display.fillCircle(233, 6, 3, connected ? TFT_GREEN : TFT_ORANGE);
    display.drawFastHLine(0, 14, display.width(), kUiRed);
}

template <typename Surface>
void drawPocketFooter(Surface& display, const char* text)
{
    display.drawFastHLine(0, 120, display.width(), kUiRule);
    display.setTextColor(kUiMuted, kUiBg);
    display.setCursor(4, 125);
    display.print(text);
}

void drawHermesBadge(M5Canvas& canvas, const std::uint8_t* pixels,
                     int originX, int originY)
{
    // Inverted plate: the paper index becomes the panel background and the
    // line-art index becomes warm ink, so the portrait sits on the graphite
    // surface instead of inside a white card.
    const std::uint16_t colors[] = {kUiBg, kUiInk, kUiMuted, kUiBg};
    int pixel = 0;
    constexpr std::size_t kFrameBytes =
        kHermesBadgeWidth * kHermesBadgeHeight / 4;
    for (std::size_t index = 0; index < kFrameBytes; ++index) {
        const std::uint8_t packed = pixels[index];
        for (int shift = 6; shift >= 0; shift -= 2, ++pixel) {
            const int x = pixel % kHermesBadgeWidth;
            const int y = pixel / kHermesBadgeWidth;
            canvas.drawPixel(originX + x, originY + y,
                             colors[(packed >> shift) & 0x03]);
        }
    }
}

}  // namespace

String singleLine(String value)
{
    value.trim();
    String result;
    result.reserve(value.length());
    bool pendingSpace = false;
    for (std::size_t index = 0; index < value.length(); ++index) {
        const unsigned char raw = static_cast<unsigned char>(value[index]);
        if ((raw & 0xC0U) == 0x80U) continue;  // UTF-8 continuation byte
        // Font 1 has ASCII glyphs only; fold other code points to '?'.
        const char character = raw < 0x80U ? static_cast<char>(raw) : '?';
        if (character == '\n' || character == '\r' || character == '\t' ||
            character == ' ') {
            pendingSpace = result.length() > 0;
            continue;
        }
        if (pendingSpace) {
            result += ' ';
            pendingSpace = false;
        }
        result += character;
    }
    return result;
}

// Draws up to maxRows word-wrapped rows of text at (4, y) on a 9 px pitch.
// With tail=false the first rows are shown; otherwise the last rows, which
// is what a growing draft needs.
template <typename Surface>
void drawWrappedTail(Surface& display, const String& text, std::size_t cols,
                     std::size_t maxRows, int y, std::uint16_t ink,
                     std::uint16_t background, bool head = false)
{
    std::vector<String> rows;
    wrapMonospace(text.c_str(), text.length(), cols,
                  [&](const char* row, std::size_t length) {
                      String line;
                      line.reserve(length);
                      for (std::size_t k = 0; k < length; ++k) line += row[k];
                      rows.push_back(line);
                  });
    std::size_t first = 0;
    if (!head && rows.size() > maxRows) first = rows.size() - maxRows;
    display.setTextColor(ink, background);
    for (std::size_t index = first; index < rows.size() && index - first < maxRows;
         ++index) {
        display.setCursor(4, y + static_cast<int>(index - first) * 9);
        display.print(rows[index]);
    }
}

const char* activityLabel(const String& status)
{
    if (status.indexOf("APPROVAL") >= 0 || status.indexOf("QUESTION") >= 0)
        return "WAIT";
    if (status.indexOf("TOOL") >= 0 || status.indexOf("SUBAGENT") >= 0)
        return "TOOL";
    if (status.indexOf("THINK") >= 0 || status.indexOf("RESPOND") >= 0 ||
        status.indexOf("STARTING") >= 0)
        return "WORK";
    if (status.indexOf("ERROR") >= 0 || status.indexOf("FAILED") >= 0)
        return "ERR";
    return "READY";
}

M5Canvas* fullScreenCanvas()
{
    static M5Canvas canvas(&M5Cardputer.Display);
    static bool attempted = false;
    static bool ready = false;
    if (!attempted) {
        attempted = true;
        canvas.setColorDepth(16);
        ready = canvas.createSprite(M5Cardputer.Display.width(),
                                    M5Cardputer.Display.height()) != nullptr;
    }
    return ready ? &canvas : nullptr;
}

String App::shortText(const String& value, std::size_t maxLength) const
{
    return value.length() <= maxLength ? value : value.substring(0, maxLength - 1) + "~";
}

void App::draw()
{
    dirty_ = false;
    if (screenSleep_) {
        drawScreenSleep();
        return;
    }
    if (screen_ == Screen::kSessions) {
        drawSessionsScreen();
        return;
    }
    if (screen_ == Screen::kRecording) {
        drawRecordingScreen();
        return;
    }
    if (screen_ == Screen::kHelpSettings) {
        drawHelpSettingsScreen();
        return;
    }
#if HERMES_WIFI_SETUP
    if (screen_ == Screen::kWifi) {
        drawWifiScreen();
        return;
    }
#endif
    auto& display = M5Cardputer.Display;
    display.fillScreen(kUiBg);
    const char* section = screen_ == Screen::kChat ? "CHAT"
                          : screen_ == Screen::kCompose ? "TYPE"
                          : screen_ == Screen::kInteraction ? "INPUT"
                          : "AUDIO";
    drawPocketHeader(display, section, hermes_.connected());
    const char* activity = activityLabel(status_);
    const bool showStatus = strcmp(activity, "READY") != 0;
    display.setTextColor(kUiRed, kUiBg);
    display.setCursor(4, 18);
    display.print(shortText(screen_ == Screen::kChat &&
                                    activeSessionTitle_.length() && !showStatus
                                ? activeSessionTitle_ : status_, 24));
    if (usageText_.length()) {
        display.setTextColor(kUiMuted, kUiBg);
        display.setCursor(162, 18);
        display.print(shortText(usageText_, 5));
    }
    display.fillRoundRect(199, 17, 37, 9, 2, kUiRedDark);
    display.setTextColor(kUiInk, kUiRedDark);
    display.setCursor(203, 18);
    display.print(activity);
    display.setTextColor(kUiInk, kUiBg);
    drawBackgroundAccents(display);

    if (screen_ == Screen::kChat) {
        if ((resumeRequestId_ && !activeSessionId_.length()) ||
            (historySyncPending_ && !timelineFromCache_)) {
            display.fillRoundRect(5, 34, 230, 76, 3, kUiPanel);
            display.drawRoundRect(5, 34, 230, 76, 3, kUiRule);
            display.fillRect(5, 34, 4, 76, kUiRed);
            display.setTextColor(kUiRed, kUiPanel);
            display.setCursor(18, 45);
            display.print("LOADING SESSION");
            display.setTextColor(kUiInk, kUiPanel);
            display.setCursor(18, 62);
            display.print(shortText(activeSessionTitle_, 32));
            display.setTextColor(kUiMuted, kUiPanel);
            display.setCursor(18, 80);
            display.print(historySyncPending_ ? "Caching history to SD..."
                                              : "Restoring Hermes context...");
            for (int block = 0; block < 8; ++block) {
                display.drawRect(18 + block * 14, 96, 10, 5, kUiRule);
                if (block < 3) {
                    display.fillRect(19 + block * 14, 97, 8, 3, kUiRed);
                }
            }
            drawPocketFooter(display, "ESC CANCEL                 PLEASE WAIT");
            return;
        }
        display.setCursor(4, 34);
        display.setTextColor(kUiInk, kUiBg);
        // 38 columns keep the last glyph clear of the 3 px scrollbar.
        std::vector<String> lines;
        wrapMonospace(timeline_.c_str(), timeline_.length(), 38,
                      [&](const char* row, std::size_t length) {
                          String text;
                          text.reserve(length);
                          for (std::size_t k = 0; k < length; ++k) text += row[k];
                          lines.push_back(text);
                      });
        const int visible = 10;
        timelineMaxScroll_ = max(0, static_cast<int>(lines.size()) - visible);
        scroll_ = clampTimelineScroll(scroll_, timelineMaxScroll_);
        const int start = scroll_;
        for (int row = 0; row < visible && start + row < static_cast<int>(lines.size()); ++row) {
            display.setCursor(4, 32 + row * 9);
            const String& shown = lines[start + row];
            if (shown.startsWith("YOU: ")) {
                display.setTextColor(kUiRed, kUiBg);
                display.print("YOU /");
                display.setTextColor(kUiInk, kUiBg);
                display.setCursor(34, 32 + row * 9);
                display.print(shown.substring(5));
            } else if (shown.startsWith("HERMES: ")) {
                display.setTextColor(kUiMuted, kUiBg);
                display.print("HERMES /");
                display.setTextColor(kUiInk, kUiBg);
                display.setCursor(52, 32 + row * 9);
                display.print(shown.substring(8));
            } else {
                display.setTextColor(shown.startsWith("[") ? kUiMuted : kUiInk,
                                     kUiBg);
                display.print(shown);
            }
        }
        if (static_cast<int>(lines.size()) > visible) {
            const int maximum = static_cast<int>(lines.size()) - visible;
            const int thumbHeight = max(7, 84 * visible /
                                           static_cast<int>(lines.size()));
            const int thumbY = 32 + (84 - thumbHeight) * start / maximum;
            display.drawFastVLine(239, 32, 84, kUiRule);
            display.fillRect(237, thumbY, 3, thumbHeight, kUiRed);
        }
        drawPocketFooter(display, voiceRetryAvailable_
                                      ? "V RETRY VOICE  DEL DISCARD  ` LIST"
                                  : turnInProgress_
                                      ? "S STEER  X STOP  ^v SCROLL  ` LIST"
                                      : "T TYPE  V VOICE  R READ  / CMD  ` LIST");
    } else if (screen_ == Screen::kCompose) {
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(4, 34);
        display.print(composeMode_ == ComposeMode::kSteer ? "STEER" : "PROMPT / COMMAND");
        display.setTextColor(kUiInk, kUiBg);
        // Eight 9 px rows fit between the label and the footer rule; only
        // the tail of a long draft is shown, with the cursor wrapped inline.
        drawWrappedTail(display, compose_ + "_", 39, 8, 47, kUiInk, kUiBg);
        drawPocketFooter(display, "ENTER SEND                 ` CANCEL");
    } else if (screen_ == Screen::kInteraction) {
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(4, 34);
        display.print(shortText(interactionType_, 36));
        display.setTextColor(kUiInk, kUiBg);
        const bool approval = interactionType_ == "approval.request";
        drawWrappedTail(display, interactionPrompt_, 39, approval ? 7 : 6, 47,
                        kUiInk, kUiBg, true);
        if (!approval) {
            // The typed answer used to be invisible: show it with a cursor.
            String answer = compose_;
            if (interactionType_ == "secret.request" ||
                interactionType_ == "sudo.request") {
                for (std::size_t k = 0; k < answer.length(); ++k) answer[k] = '*';
            }
            answer += "_";
            if (answer.length() > 36) answer = answer.substring(answer.length() - 36);
            display.fillRoundRect(2, 103, 236, 14, 2, kUiPanel);
            display.fillRect(2, 103, 3, 14, kUiRed);
            display.setTextColor(kUiRed, kUiPanel);
            display.setCursor(9, 106);
            display.print(">");
            display.setTextColor(kUiInk, kUiPanel);
            display.setCursor(18, 106);
            display.print(answer);
        }
        String footer;
        if (approval) {
            if (approvalChoices_.indexOf(",once,") >= 0) footer += "O ONCE  ";
            if (approvalChoices_.indexOf(",session,") >= 0) footer += "S SESSION  ";
            if (approvalChoices_.indexOf(",always,") >= 0) footer += "A ALWAYS  ";
            if (approvalChoices_.indexOf(",deny,") >= 0) footer += "D DENY";
        } else {
            footer = "TYPE ANSWER  ENTER SEND  ` CANCEL";
        }
        drawPocketFooter(display, footer.c_str());
    } else {
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(4, 40);
        display.setTextFont(2);
        display.print("HERMES SPEAKING");
        display.setTextFont(1);
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(4, 70);
        display.print("Synthesizing / playing audio...");
        drawPocketFooter(display, "ESC CANCEL");
    }
}

void App::drawRecordingScreen()
{
    auto& display = M5Cardputer.Display;
    M5Canvas* canvas = fullScreenCanvas();
    if (!canvas) {
        display.fillScreen(kUiBg);
        display.setTextFont(2);
        display.setTextColor(kUiRed, kUiBg);
        display.setCursor(4, 48);
        display.print(status_);
        return;
    }

    const unsigned long elapsed = voice_.elapsedMs();
    const unsigned long seconds = elapsed / 1000;
    const unsigned long tenths = (elapsed % 1000) / 100;
    const bool listening = voice_.active() && !status_.startsWith("FINALIZING");

    canvas->fillScreen(kUiBg);
    canvas->setTextWrap(false);
    canvas->setTextFont(1);
    canvas->setTextColor(kUiInk, kUiBg);
    canvas->setCursor(4, 3);
    canvas->print("HERMES // VOICE");
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(199, 3);
    canvas->print("LINK");
    canvas->fillCircle(233, 6, 3,
                       hermes_.connected() ? TFT_GREEN : TFT_ORANGE);
    canvas->drawFastHLine(0, 14, display.width(), kUiRed);

    canvas->fillRoundRect(5, 22, 230, 88, 3, kUiPanel);
    canvas->drawRoundRect(5, 22, 230, 88, 3, listening ? kUiRed : kUiRule);
    if (listening) {
        canvas->fillCircle(27, 48, 7, kUiRed);
        canvas->drawCircle(27, 48, 10, kUiRedDark);
    } else {
        canvas->fillCircle(30, 48, 7, TFT_ORANGE);
    }

    canvas->setTextFont(2);
    canvas->setTextColor(listening ? kUiRed : kUiMuted, kUiPanel);
    canvas->setCursor(45, 37);
    canvas->print(listening ? String(voiceTest_ ? "MIC TEST" : "RECORDING")
                            : status_);

    canvas->setTextFont(1);
    canvas->setTextColor(kUiInk, kUiPanel);
    canvas->setCursor(45, 59);
    if (listening) {
        canvas->printf("%02lu:%02lu.%lu  /  00:%02lu",
                       seconds / 60,
                       seconds % 60,
                       tenths,
                       kMaxVoiceMs / 1000);
    } else {
        canvas->print("PLEASE WAIT");
    }

    canvas->setTextColor(kUiMuted, kUiPanel);
    canvas->setCursor(15, 76);
    canvas->print("MIC");
    const std::uint8_t level = listening ? voice_.levelBars() : 0;
    for (int bar = 0; bar < 12; ++bar) {
        const int x = 45 + bar * 15;
        canvas->drawRect(x, 76, 11, 7, kUiRule);
        if (bar < level) canvas->fillRect(x + 1, 77, 9, 5, kUiRed);
    }

    canvas->drawRoundRect(15, 91, 210, 8, 3, kUiRule);
    if (listening) {
        const unsigned long rawProgress = 206 * elapsed / kMaxVoiceMs;
        const int progress = static_cast<int>(rawProgress > 206 ? 206 : rawProgress);
        if (progress > 0) canvas->fillRect(17, 93, progress, 4, kUiRed);
    }
    canvas->setTextColor(kUiMuted, kUiPanel);
    canvas->setCursor(15, 102);
    canvas->print(listening ? "REC / MAX 30 SEC" : "PROCESSING AUDIO");

    canvas->drawFastHLine(0, 120, display.width(), kUiRule);
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(4, 125);
    canvas->print(listening
                      ? (voiceTest_ ? "ENTER DONE            ESC CANCEL"
                                    : "ENTER SEND            ESC CANCEL")
                      : "ESC CANCEL              PROCESSING");

    // Present one complete LCD frame; no visible clear/draw passes.
    canvas->pushSprite(0, 0);
}

void App::drawHelpSettingsScreen()
{
    auto& display = M5Cardputer.Display;
    display.fillScreen(kUiBg);
    drawPocketHeader(display, helpPage_ == 0 ? "MANUAL"
                              : helpPage_ == 1 ? "SETUP" : "STATUS",
                     hermes_.connected());
    display.setTextColor(kUiInk, kUiBg);
    display.setCursor(4, 20);
    drawBackgroundAccents(display);
    if (helpPage_ == 0) {
        // Five 17 px cards on a 19 px pitch fit between the header and the
        // footer rule, so every chat control has a hint somewhere on-device.
        auto drawHelpCard = [&](int y, const char* title) {
            display.fillRoundRect(3, y, 234, 17, 3, kUiPanel);
            display.fillRect(3, y + 2, 3, 13, kUiRed);
            display.drawFastVLine(39, y + 3, 11, kUiRule);
            display.setTextColor(kUiRed, kUiPanel);
            display.setCursor(9, y + 5);
            display.print(title);
        };
        auto drawKeyHint = [&](int x, int y, const char* key,
                               const char* action) {
            const int keyWidth = static_cast<int>(strlen(key)) * 6 + 6;
            display.fillRoundRect(x, y, keyWidth, 12, 2, kUiRedDark);
            display.setTextColor(kUiInk, kUiRedDark);
            display.setCursor(x + 3, y + 2);
            display.print(key);
            x += keyWidth + 2;
            display.setTextColor(kUiInk, kUiPanel);
            display.setCursor(x, y + 2);
            display.print(action);
            return x + static_cast<int>(strlen(action)) * 6 + 6;
        };

        drawHelpCard(20, "LIST");
        int x = drawKeyHint(45, 23, "^v", "MOVE");
        x = drawKeyHint(x, 23, "ENT", "OPEN");
        x = drawKeyHint(x, 23, "N", "NEW");
        drawKeyHint(x, 23, "R", "SYNC");

        drawHelpCard(39, "CHAT");
        x = drawKeyHint(45, 42, "T", "TYPE");
        x = drawKeyHint(x, 42, "V", "VOICE");
        x = drawKeyHint(x, 42, "R", "READ");
        drawKeyHint(x, 42, "/", "CMD");

        drawHelpCard(58, "TURN");
        x = drawKeyHint(45, 61, "S", "STEER");
        x = drawKeyHint(x, 61, "X", "STOP");
        drawKeyHint(x, 61, "^v", "SCROLL");

        drawHelpCard(77, "SESS");
        x = drawKeyHint(45, 80, "B", "BRANCH");
        x = drawKeyHint(x, 80, "C", "COMPACT");
        drawKeyHint(x, 80, "U", "UNDO");

        drawHelpCard(96, "SYS");
        x = drawKeyHint(45, 99, "Z", "SLEEP");
        x = drawKeyHint(x, 99, "D", "STATUS");
        drawKeyHint(x, 99, "GO", "HOLD=HELP");
    } else if (helpPage_ == 1) {
        display.setTextColor(kUiMuted, kUiBg);
        display.printf("PROFILE %s / %s",
                       shortText(config_.profile.length() ? config_.profile
                                                         : String("default"), 10).c_str(),
                       shortText(hermes_.authMode(), 10).c_str());
        const char* labels[] = {"DISPLAY / AWAKE", "DISPLAY / SLEEP",
                                "DISPLAY / DIM", "AUDIO / TTS",
                                "SLEEP / MOTION", "AUDIO / ALERTS",
                                "CACHE / ENABLED", "CACHE / QUOTA"};
        const int firstSetting = min(max(static_cast<int>(settingRow_) - 4, 0), 3);
        for (int row = 0; row < 5; ++row) {
            const int setting = firstSetting + row;
            const int y = 31 + row * 17;
            const bool selected = setting == settingRow_;
            const std::uint16_t background = selected ? kUiRedDark : kUiBg;
            if (selected) {
                display.fillRoundRect(2, y - 2, 234, 15, 2, background);
                display.fillRect(2, y - 2, 3, 15, kUiRed);
            }
            display.setTextColor(selected ? kUiInk : kUiMuted, background);
            display.setCursor(8, y);
            display.print(labels[setting]);
            display.setTextColor(kUiInk, background);
            display.setCursor(181, y);
            if (setting == 0) display.printf("%3u", awakeBrightness_);
            else if (setting == 1) display.printf("%3us", config_.screenSleepSeconds);
            else if (setting == 2) display.printf("%3u", config_.screenSleepBrightness);
            else if (setting == 3) display.printf("%3u", config_.ttsVolume);
            else if (setting == 4) display.print(sleepMotion_ == 0 ? "OFF"
                                            : sleepMotion_ == 1 ? "LOW" : "HIGH");
            else if (setting == 5) display.print(alertsEnabled_ ? " ON" : "OFF");
            else if (setting == 6) display.print(cacheEnabled_ ? " ON" : "OFF");
            else display.printf("%3uM", cacheQuotaMb_);
            if (selected) {
                display.setTextColor(kUiRed, background);
                display.setCursor(169, y);
                display.print('<');
                display.setCursor(205, y);
                display.print('>');
            }
        }
    } else {
        display.printf(
            "DEVICE   HM-01 / FW %s\n\n"
            "WIFI     %s\n"
            "IP       %s\n"
            "SIGNAL   %d DBM\n"
            "GATEWAY  %s\n"
            "HERMES   %s\n"
            "CACHE    %s / %uMB / %luKB\n"
            "STATE    %s\n"
            "LAMP     GREEN LINK / ORANGE OFFLINE\n"
            "PLATE    READY WORK TOOL WAIT ERR",
            kFirmwareBuild,
            shortText(WiFi.SSID(), 25).c_str(),
            WiFi.localIP().toString().c_str(), WiFi.RSSI(),
            shortText(config_.baseUrl, 25).c_str(),
            hermes_.connected() ? "LINKED" : "OFFLINE",
            cache_.enabled() ? "READY" :
                (cacheEnabled_ ? shortText(cache_.error(), 10).c_str() : "OFF"),
            cacheQuotaMb_,
            static_cast<unsigned long>(cache_.usageBytes() / 1024ULL),
            shortText(status_, 25).c_str());
    }
    if (cacheClearConfirm_) {
        display.drawFastHLine(0, 120, display.width(), kUiRule);
        display.fillRect(0, 121, display.width(), 14, kUiRedDark);
        display.setTextColor(kUiInk, kUiRedDark);
        display.setCursor(4, 125);
        display.print("C AGAIN = ERASE CACHE   ANY KEY KEEPS");
        return;
    }
    drawPocketFooter(display, helpPage_ == 0
                                  ? "S SETUP  D STATUS  ESC / GO BACK"
                              : helpPage_ == 1
#if HERMES_WIFI_SETUP
                                  ? "^v <> EDIT  C CLEAR  W WIFI  ESC SAVE"
                                  : "M MIC  K SPKR  R RELINK  W WIFI  H HELP");
#else
                                  ? "^v ITEM  <> CHANGE  C CLEAR  ESC SAVE"
                                  : "M MIC  K SPEAKER  R RECONNECT  H HELP");
#endif
}

void App::drawSessionsScreen()
{
    M5Canvas* canvas = fullScreenCanvas();
    if (!canvas) {
        auto& lcd = M5Cardputer.Display;
        lcd.fillScreen(kUiBg);
        drawPocketHeader(lcd, "SESSIONS", hermes_.connected());
        lcd.setTextColor(kUiInk, kUiBg);
        lcd.setCursor(8, 40);
        lcd.print(shortText(status_, 36));
        return;
    }
    auto& display = *canvas;
    constexpr int kVisibleRows = 5;
    constexpr int kListTop = 30;
    constexpr int kRowHeight = 18;

    display.fillScreen(kUiBg);
    display.setTextFont(1);
    display.setTextWrap(false);

    drawPocketHeader(display, "SESSIONS", hermes_.connected());
    drawBackgroundAccents(display);

    const char* footer = "^v MOVE  ENTER OPEN  N NEW  V VOICE";
    if (sessions_.empty()) {
        const bool loadingSessions = sessionsSyncPending_ ||
                                     (hermes_.connected() && sessionsRequestId_);
        const bool localError = status_.indexOf("ERROR") >= 0 ||
                                status_.indexOf("FAILED") >= 0 ||
                                status_.indexOf("INVALID") >= 0 ||
                                status_.indexOf("REQUIRED") >= 0;
        const bool connectionError = hermes_.connectionFailed() || localError;
        if (!hermes_.connected() || loadingSessions || connectionError) {
            display.fillRoundRect(5, 35, 230, 70, 3, kUiPanel);
            display.drawRoundRect(5, 35, 230, 70, 3,
                                  connectionError ? kUiRed : kUiRule);
            display.fillRect(5, 35, 4, 70, kUiRed);
            display.setTextColor(kUiRed, kUiPanel);
            display.setCursor(18, 46);
            display.print(connectionError ? "CONNECTION ERROR"
                          : loadingSessions ? "LOADING SESSIONS"
                                            : "CONNECTING TO HERMES");
            display.setTextColor(connectionError ? kUiInk : kUiMuted,
                                 kUiPanel);
            display.setCursor(18, 64);
            display.print(shortText(connectionError
                                        ? (hermes_.connectionFailed()
                                               ? hermes_.diagnostic() : status_)
                                        : config_.baseUrl, 34));
            display.setTextColor(kUiMuted, kUiPanel);
            display.setCursor(18, 82);
            display.print(connectionError ? "Automatic retry is active"
                          : loadingSessions ? "Waiting for session list..."
                                            : "Authorizing secure link...");
            if (!connectionError) {
                const int lit = 1 + (millis() / 180) % 8;
                for (int block = 0; block < 8; ++block) {
                    display.drawRect(18 + block * 14, 95, 10, 5, kUiRule);
                    if (block < lit) {
                        display.fillRect(19 + block * 14, 96, 8, 3, kUiRed);
                    }
                }
            }
            footer = connectionError ? "R RETRY  D DETAILS  HOLD GO HELP"
                     : "PLEASE WAIT              D DETAILS";
        } else {
            display.setTextColor(kUiMuted, kUiBg);
            display.setCursor(4, 19);
            display.print("NO SAVED SESSIONS");
            display.setTextColor(kUiRed, kUiBg);
            display.setCursor(61, 61);
            display.print("PRESS N TO CREATE");
        }
    } else {
        const int count = static_cast<int>(sessions_.size());
        const int selected = min(max(selectedSession_, 0), count - 1);
        const int maxStart = max(0, count - kVisibleRows);
        const int windowStart = min(max(selected - kVisibleRows / 2, 0), maxStart);
        const int windowEnd = min(count, windowStart + kVisibleRows);

        display.setTextColor(kUiMuted, kUiBg);
        display.setCursor(4, 18);
        const int total = sessionsTotal_ ? static_cast<int>(sessionsTotal_) : count;
        const int globalSelected = static_cast<int>(sessionsWindowOffset_) + selected;
        display.printf("SESSIONS / %d", total);
        display.setCursor(174, 18);
        display.printf("%d/%d", globalSelected + 1, total);

        for (int index = windowStart; index < windowEnd; ++index) {
            const int row = index - windowStart;
            const int y = kListTop + row * kRowHeight;
            const bool isSelected = index == selected;
            const std::uint16_t background = isSelected ? kUiRedDark : kUiBg;

            if (isSelected) {
                display.fillRoundRect(2, y, 234, kRowHeight - 1, 2, background);
                display.fillRect(2, y, 3, kRowHeight - 1, kUiRed);
            } else {
                display.drawFastHLine(22, y + kRowHeight - 1, 212, kUiRule);
            }

            display.setTextColor(isSelected ? kUiInk : kUiRed, background);
            display.setCursor(5, y + 1);
            display.printf("%4d", static_cast<int>(sessionsWindowOffset_) + index + 1);

            const String title = singleLine(sessions_[index].title);
            display.setTextColor(kUiInk, background);
            display.setCursor(35, y + 1);
            display.print(shortText(title.length() ? title : String("Untitled"), 33));

            String metadata = singleLine(sessions_[index].preview);
            String sessionState = singleLine(sessions_[index].state);
            sessionState.toUpperCase();
            if (sessionState.length()) {
                metadata = shortText(sessionState, 8) + " / " + metadata;
            }
            if (!metadata.length() || metadata == title) {
                const String& id = sessions_[index].id;
                metadata = "SAVED  ";
                metadata += id.length() > 8 ? id.substring(id.length() - 8) : id;
            }
            display.setTextColor(isSelected ? kUiInk : kUiMuted, background);
            display.setCursor(35, y + 9);
            display.print(shortText(metadata, 33));
        }

        if (count > kVisibleRows) {
            constexpr int kTrackTop = kListTop;
            constexpr int kTrackHeight = kVisibleRows * kRowHeight - 1;
            const int thumbHeight = max(8, kTrackHeight * kVisibleRows / count);
            const int thumbTravel = kTrackHeight - thumbHeight;
            const int thumbY = kTrackTop +
                               (maxStart ? thumbTravel * windowStart / maxStart : 0);
            display.drawFastVLine(239, kTrackTop, kTrackHeight, kUiRule);
            display.fillRect(237, thumbY, 3, thumbHeight, kUiRed);
        }
    }

    if (sessionsSyncPending_) footer = "ESC CANCEL              SYNCING SD";
    drawPocketFooter(display, footer);
    // The animated connection rail is composed off-screen. The LCD only sees
    // completed frames, so its periodic update cannot flash a cleared screen.
    canvas->pushSprite(0, 0);
}

#if HERMES_WIFI_SETUP
void App::drawWifiScreen()
{
    M5Canvas* canvas = fullScreenCanvas();
    auto& lcd = M5Cardputer.Display;
    if (!canvas) {
        lcd.fillScreen(kUiBg);
        drawPocketHeader(lcd, "WIFI", hermes_.connected());
        lcd.setTextColor(kUiInk, kUiBg);
        lcd.setCursor(8, 40);
        lcd.print(shortText(wifiNotice_.length() ? wifiNotice_ : status_, 36));
        return;
    }
    auto& display = *canvas;
    constexpr int kVisibleRows = 5;
    constexpr int kListTop = 30;
    constexpr int kRowHeight = 18;
    const String current = WiFi.SSID();

    display.fillScreen(kUiBg);
    display.setTextFont(1);
    display.setTextWrap(false);
    drawPocketHeader(display, "WIFI", hermes_.connected());
    drawBackgroundAccents(display);

    auto drawPanel = [&](const char* title, const String& line,
                         const char* hint, bool animate) {
        display.fillRoundRect(5, 35, 230, 70, 3, kUiPanel);
        display.drawRoundRect(5, 35, 230, 70, 3, kUiRule);
        display.fillRect(5, 35, 4, 70, kUiRed);
        display.setTextColor(kUiRed, kUiPanel);
        display.setCursor(18, 46);
        display.print(title);
        display.setTextColor(kUiInk, kUiPanel);
        display.setCursor(18, 64);
        display.print(shortText(line, 34));
        display.setTextColor(kUiMuted, kUiPanel);
        display.setCursor(18, 82);
        display.print(hint);
        if (animate) {
            const int lit = 1 + (millis() / 180) % 8;
            for (int block = 0; block < 8; ++block) {
                display.drawRect(18 + block * 14, 95, 10, 5, kUiRule);
                if (block < lit) display.fillRect(19 + block * 14, 96, 8, 3, kUiRed);
            }
        }
    };

    const char* footer = "^v MOVE  ENTER JOIN  R SCAN  ESC BACK";
    display.setCursor(4, 18);
    if (wifiPhase_ == WifiPhase::kScanning) {
        display.setTextColor(kUiRed, kUiBg);
        display.print("SCANNING");
        drawPanel("SCANNING NETWORKS", current.length() ? "On " + current : String("Not connected"),
                  "Listing 2.4 GHz access points...", true);
        footer = "PLEASE WAIT                ESC BACK";
    } else if (wifiPhase_ == WifiPhase::kJoining) {
        display.setTextColor(kUiRed, kUiBg);
        display.print("JOINING");
        drawPanel("JOINING NETWORK", wifiTargetSsid_,
                  "Authenticating and waiting for DHCP...", true);
        footer = "ESC CANCEL                PLEASE WAIT";
    } else if (wifiPhase_ == WifiPhase::kPassword) {
        display.setTextColor(kUiRed, kUiBg);
        display.print("PASSWORD");
        display.fillRoundRect(5, 35, 230, 70, 3, kUiPanel);
        display.drawRoundRect(5, 35, 230, 70, 3, kUiRed);
        display.fillRect(5, 35, 4, 70, kUiRed);
        display.setTextColor(kUiRed, kUiPanel);
        display.setCursor(18, 44);
        display.print(shortText("KEY FOR " + wifiTargetSsid_, 34));
        display.setTextColor(kUiMuted, kUiPanel);
        display.setCursor(18, 56);
        display.print("Typed in the clear; DEL erases.");
        // Two 34-column rows show the tail of the key with the cursor.
        std::vector<String> rows;
        const String typed = compose_ + "_";
        wrapMonospace(typed.c_str(), typed.length(), 34,
                      [&](const char* row, std::size_t length) {
                          String line;
                          for (std::size_t k = 0; k < length; ++k) line += row[k];
                          rows.push_back(line);
                      });
        const std::size_t first = rows.size() > 2 ? rows.size() - 2 : 0;
        display.setTextColor(kUiInk, kUiPanel);
        for (std::size_t index = first; index < rows.size(); ++index) {
            display.setCursor(18, 72 + static_cast<int>(index - first) * 11);
            display.print(rows[index]);
        }
        footer = "ENTER JOIN                 ESC BACK";
    } else if (wifiNetworks_.empty()) {
        display.setTextColor(kUiRed, kUiBg);
        display.print(wifiNotice_.length() ? shortText(wifiNotice_, 32) : String("NO NETWORKS"));
        drawPanel("NO NETWORKS LISTED", current.length() ? "On " + current : String("Not connected"),
                  "R scans again. Only 2.4 GHz is supported.", false);
        footer = "R SCAN                     ESC BACK";
    } else {
        const int count = static_cast<int>(wifiNetworks_.size());
        const int selected = min(max(selectedWifi_, 0), count - 1);
        const int maxStart = max(0, count - kVisibleRows);
        const int windowStart = min(max(selected - kVisibleRows / 2, 0), maxStart);
        const int windowEnd = min(count, windowStart + kVisibleRows);
        if (wifiNotice_.length()) {
            display.setTextColor(kUiRed, kUiBg);
            display.print(shortText(wifiNotice_, 26));
        } else {
            display.setTextColor(kUiMuted, kUiBg);
            display.printf("NETWORKS / %d", count);
        }
        display.setTextColor(kUiMuted, kUiBg);
        display.setCursor(168, 18);
        display.print(current.length() ? shortText("ON " + current, 11) : String("NO LINK"));

        for (int index = windowStart; index < windowEnd; ++index) {
            const WifiNetwork& network = wifiNetworks_[index];
            const int row = index - windowStart;
            const int y = kListTop + row * kRowHeight;
            const bool isSelected = index == selected;
            const std::uint16_t background = isSelected ? kUiRedDark : kUiBg;
            if (isSelected) {
                display.fillRoundRect(2, y, 234, kRowHeight - 1, 2, background);
                display.fillRect(2, y, 3, kRowHeight - 1, kUiRed);
            } else {
                display.drawFastHLine(22, y + kRowHeight - 1, 212, kUiRule);
            }
            // Four-bar signal meter: -55 dBm and better lights every bar.
            const int bars = network.rssi > -55 ? 4 : network.rssi > -65 ? 3
                           : network.rssi > -75 ? 2 : 1;
            for (int bar = 0; bar < 4; ++bar) {
                const int height = 3 + bar * 3;
                const int x = 9 + bar * 5;
                if (bar < bars) display.fillRect(x, y + 14 - height, 3, height, isSelected ? kUiInk : kUiRed);
                else display.drawRect(x, y + 14 - height, 3, height, kUiRule);
            }
            display.setTextColor(kUiInk, background);
            display.setCursor(35, y + 1);
            display.print(shortText(singleLine(network.ssid), 33));
            String meta = String(static_cast<int>(network.rssi)) + " DBM  ";
            meta += network.secured ? "LOCKED" : "OPEN";
            if (network.ssid == current) meta += "  JOINED";
            else if (network.ssid == wifiSavedSsid_) meta += "  SAVED";
            display.setTextColor(isSelected ? kUiInk : kUiMuted, background);
            display.setCursor(35, y + 9);
            display.print(shortText(meta, 33));
        }
        if (count > kVisibleRows) {
            constexpr int kTrackHeight = kVisibleRows * kRowHeight - 1;
            const int thumbHeight = max(8, kTrackHeight * kVisibleRows / count);
            const int thumbY = kListTop + (maxStart ? (kTrackHeight - thumbHeight) * windowStart / maxStart : 0);
            display.drawFastVLine(239, kListTop, kTrackHeight, kUiRule);
            display.fillRect(237, thumbY, 3, thumbHeight, kUiRed);
        }
        if (wifiSavedSsid_.length()) footer = "^v ENTER JOIN  R SCAN  DEL FORGET  ESC";
    }
    drawPocketFooter(display, footer);
    canvas->pushSprite(0, 0);
}
#endif  // HERMES_WIFI_SETUP

void App::drawScreenSleep()
{
    auto& display = M5Cardputer.Display;
    M5Canvas* canvas = fullScreenCanvas();
    if (!canvas) {
        display.fillScreen(kUiBg);
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(8, 8);
        display.print(hermes_.connected() ? "HERMES ONLINE" : "HERMES OFFLINE");
        display.fillCircle(228, 10, 3,
                           hermes_.connected() ? TFT_GREEN : TFT_ORANGE);
        display.setTextColor(kUiInk, kUiBg);
        display.setCursor(8, 28);
        display.print(shortText(status_, 36));
        return;
    }

    canvas->fillScreen(kUiBg);
    canvas->drawFastVLine(99, 17, 101, kUiRule);
    canvas->fillRect(97, 24, 5, 2, kUiRed);
    canvas->fillRect(97, 104, 5, 2, kUiRed);
    const int phase = (sleepFrame_ / 10) % 4;
    const int rawBob = phase <= 2 ? phase : 4 - phase;
    const int bob = sleepMotion_ == 2 ? rawBob
                    : sleepMotion_ == 1 ? (rawBob + 1) / 2 : 0;
    const char* sleepActivity = activityLabel(status_);
    const bool workingPortrait = strcmp(sleepActivity, "WORK") == 0 ||
                                 strcmp(sleepActivity, "TOOL") == 0;
    const bool blinkPortrait = sleepMotion_ && !workingPortrait &&
                               strcmp(sleepActivity, "READY") == 0 &&
                               sleepFrame_ % 80 >= 72;
    const std::uint8_t* portrait = kHermesBadge2Bpp;
#if HERMES_SLEEP_STATES
    if (workingPortrait) {
        portrait = kHermesWorking2Bpp;
    } else if (blinkPortrait) {
        portrait = kHermesBlink2Bpp;
    }
#else
    (void)blinkPortrait;
#endif
    // READY and blink share a fixed origin. Only the WORK/TOOL expression may
    // use the subtle configured motion, so blinking cannot shift the portrait.
    drawHermesBadge(*canvas, portrait, 5, 20 +
#if HERMES_SLEEP_STATES
                    (workingPortrait ? bob : 0)
#else
                    0
#endif
                    );

    canvas->setTextFont(2);
    canvas->setTextColor(kUiInk, kUiBg);
    canvas->setCursor(106, 14);
    canvas->print("HERMES");
    canvas->setTextFont(1);
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(107, 36);
    canvas->print(hermes_.connected() ? "LINK / ONLINE" : "LINK / OFFLINE");
    canvas->fillCircle(226, 39, 3,
                       hermes_.connected() ? TFT_GREEN : TFT_ORANGE);

    // The right column is 21 columns wide (x 107..233). Status and the
    // session title each get two word-wrapped rows so a normal title such as
    // "Cloud backup daily status" is shown in full.
    canvas->setTextColor(kUiInk, kUiBg);
    const String compactStatus = shortText(status_, 42);
    canvas->setCursor(107, 50);
    canvas->print(compactStatus.substring(0, 21));
    if (compactStatus.length() > 21) {
        canvas->setCursor(107, 60);
        canvas->print(compactStatus.substring(21));
    }
    canvas->fillRoundRect(106, 74, 45, 11, 2, kUiRedDark);
    canvas->setTextColor(kUiInk, kUiRedDark);
    canvas->setCursor(111, 76);
    canvas->print(activityLabel(status_));
    {
        const String title = activeSessionTitle_.length()
                                 ? singleLine(activeSessionTitle_)
                                 : String("SESSION LIST");
        std::vector<String> rows;
        wrapMonospace(title.c_str(), title.length(), 21,
                      [&](const char* row, std::size_t length) {
                          String line;
                          for (std::size_t k = 0; k < length; ++k) line += row[k];
                          rows.push_back(line);
                      });
        if (rows.size() > 2) {
            rows.resize(2);
            rows[1] = shortText(rows[1] + "~~", 21);
        }
        canvas->setTextColor(kUiMuted, kUiBg);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            canvas->setCursor(107, 90 + static_cast<int>(index) * 10);
            canvas->print(rows[index]);
        }
    }
    canvas->drawFastHLine(104, 111, 129, kUiRed);
    canvas->setTextColor(kUiMuted, kUiBg);
    canvas->setCursor(107, 117);
    canvas->print("ANY KEY TO WAKE");

    // Push the complete frame at once; the physical LCD never sees a clear pass.
    canvas->pushSprite(0, 0);
}

}  // namespace hermes_terminal
