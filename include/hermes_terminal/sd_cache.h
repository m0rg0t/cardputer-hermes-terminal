#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <vector>

#include "hermes_terminal/config.h"

namespace hermes_terminal {

struct CachedSession {
    String id;
    String title;
    String preview;
    String state;
};

class SdCache {
public:
    bool begin(fs::FS& fs, const Config& config);
    void update();
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; }
    void setQuotaMb(std::uint16_t quotaMb) { quotaMb_ = quotaMb; }
    void protectSession(const String& sessionId) {
        protectedStem_ = sessionId.length() ? sessionStem(sessionId) : "";
    }
    std::uint64_t usageBytes();
    bool enforceQuota();
    const String& error() const { return error_; }
    const String& namespacePath() const { return namespacePath_; }

    bool loadSessions(std::vector<CachedSession>& sessions,
                      std::size_t limit = 12, std::size_t offset = 0);
    std::size_t sessionCount();
    bool replaceSessions(JsonArrayConst sessions);
    bool beginSessionsImport();
    bool appendSessionsImportPage(const String& responsePath,
                                  std::size_t& records);
    bool commitSessionsImport();
    void abortSessionsImport();
    bool loadTimelineWindow(const String& sessionId, std::size_t requestedStart,
                            String& timeline, String& lastAssistant,
                            std::size_t& windowStart, std::size_t& windowEnd,
                            std::size_t& totalBytes);
    bool replaceHistory(const String& sessionId, JsonArrayConst messages);
    bool replaceHistoryFromRest(const String& sessionId,
                                const String& responsePath);
    bool beginHistoryImport(const String& sessionId);
    bool appendHistoryImportPage(const String& responsePath,
                                 std::size_t& records);
    bool commitHistoryImport();
    void abortHistoryImport();
    String restStagingPath() const { return namespacePath_ + "/rest-page.tmp"; }
    bool appendMessage(const String& sessionId, const char* role,
                       const String& text, const char* state = "confirmed",
                       const String& rowId = "");
    bool updateMessageState(const String& sessionId, const String& rowId,
                            const char* state);
    bool beginAssistantSpool(const String& sessionId);
    bool appendAssistantDelta(const String& delta);
    bool finalizeAssistantSpool(const String& finalText);
    void abandonAssistantSpool();
    bool clear();

#if defined(HERMES_SIM)
    // Desktop preview (sim/): scripts private state to render every screen.
    friend struct SimAccess;
#endif
private:
    bool writeManifest();
    String sessionStem(const String& sessionId) const;
    String historyPath(const String& sessionId) const;
    String viewPath(const String& sessionId) const;
    String metaPath(const String& sessionId) const;
    bool recoverPair(const String& path);
    bool replacePair(const String& temp, const String& target);
    bool replaceHistoryFiles(const String& historyTemp,
                             const String& historyTarget,
                             const String& viewTemp,
                             const String& viewTarget);
    void recoverHistoryFiles(const String& sessionId);
    bool writeHistoryMeta(const String& sessionId);
    bool verifyHistoryMeta(const String& sessionId);
    void quarantineHistory(const String& sessionId);
    bool flushAssistantSpool();
    bool recoverAssistantSpool(const String& sessionId);
    void touchSession(const String& sessionId);
    bool loadLatestAssistant(File& view, String& output);
    bool writeRecord(File& jsonl, File& view, const char* role,
                     const String& text, const char* state,
                     const String& rowId);
    bool parseRestPage(File& input, File& jsonl, File& view,
                       std::size_t& records);
    bool writeRestObject(File& input, std::size_t objectStart,
                         std::size_t objectEnd, File& jsonl, File& view,
                         const String& role, const String& rowId);
    bool streamJsonText(File& input, std::size_t objectEnd, File& jsonl,
                        File& view, const String& role, const String& rowId,
                        std::size_t& segment);
    void fail(const String& message);

    fs::FS* fs_ = nullptr;
    bool enabled_ = true;
    String namespacePath_;
    String error_;
    String spoolSessionId_;
    String spoolPath_;
    File spoolFile_;
    String spoolBuffer_;
    unsigned long spoolFlushMs_ = 0;
    std::uint16_t quotaMb_ = 32;
    String protectedStem_;
    String verifiedSessionId_;
    String importHistoryPath_;
    String importViewPath_;
    String importHistoryTemp_;
    String importViewTemp_;
    String importSessionId_;
    String sessionsImportTemp_;
};

}  // namespace hermes_terminal
