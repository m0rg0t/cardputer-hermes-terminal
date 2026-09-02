#include "hermes_terminal/sd_cache.h"

#include <mbedtls/sha256.h>
#include <cstring>

#include "hermes_terminal/cache_rules.h"

namespace hermes_terminal {
namespace {

constexpr const char* kCacheRoot = "/.HERMES-CACHE";
constexpr std::size_t kSpoolFlushBytes = 512;
constexpr unsigned long kSpoolFlushIntervalMs = 250;
constexpr std::size_t kLatestAssistantBytes = 12000;

String hexDigest(const String& value, std::size_t bytes)
{
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.c_str()),
                   value.length(), digest, 0);
    static const char hex[] = "0123456789abcdef";
    String output;
    output.reserve(bytes * 2);
    for (std::size_t index = 0; index < bytes; ++index) {
        output += hex[digest[index] >> 4];
        output += hex[digest[index] & 0x0f];
    }
    return output;
}

String messageText(JsonObjectConst item)
{
    String text = item["text"] | "";
    if (text.length()) return text;
    JsonVariantConst content = item["content"];
    if (content.is<const char*>()) return String(content.as<const char*>());
    if (content.is<JsonArrayConst>()) {
        for (JsonObjectConst part : content.as<JsonArrayConst>()) {
            const String type = part["type"] | "";
            if (type == "text" || type == "input_text" || type == "output_text") {
                if (text.length()) text += '\n';
                text += String(part["text"] | "");
            }
        }
    }
    return text;
}

String roleLabel(const String& role)
{
    if (role == "user") return "YOU: ";
    if (role == "assistant") return "HERMES: ";
    return "[" + role + "] ";
}

int hexValue(int value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool readHex4(File& input, std::uint16_t& value)
{
    value = 0;
    for (int index = 0; index < 4; ++index) {
        const int digit = hexValue(input.read());
        if (digit < 0) return false;
        value = static_cast<std::uint16_t>((value << 4) | digit);
    }
    return true;
}

void appendUtf8(String& output, std::uint32_t codepoint)
{
    if (codepoint <= 0x7f) output += static_cast<char>(codepoint);
    else if (codepoint <= 0x7ff) {
        output += static_cast<char>(0xc0 | (codepoint >> 6));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        output += static_cast<char>(0xe0 | (codepoint >> 12));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        output += static_cast<char>(0xf0 | (codepoint >> 18));
        output += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        output += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

bool readJsonToken(File& input, std::size_t end, String& token)
{
    token = "";
    bool escaped = false;
    while (input.position() < end && input.available()) {
        const char value = static_cast<char>(input.read());
        if (!escaped && value == '"') return true;
        if (!escaped && value == '\\') { escaped = true; continue; }
        if (token.length() < 24) token += value;
        escaped = false;
    }
    return false;
}

std::uint32_t fileCrc32(File& file)
{
    std::uint8_t buffer[256];
    std::uint32_t state = 0xFFFFFFFFU;
    file.seek(0);
    while (file.available()) {
        const std::size_t count = file.read(buffer, sizeof(buffer));
        if (!count) break;
        state = cacheCrc32Update(state, buffer, count);
    }
    return ~state;
}

}  // namespace

bool SdCache::begin(fs::FS& fs, const Config& config)
{
    fs_ = &fs;
    String identity = config.baseUrl;
    identity.trim();
    identity.toLowerCase();
    identity += "\n";
    identity += config.profile.length() ? config.profile : "default";
    namespacePath_ = String(kCacheRoot) + "/" + hexDigest(identity, 8);
    if (!fs_->exists(kCacheRoot) && !fs_->mkdir(kCacheRoot)) {
        fail("CACHE ROOT CREATE FAILED");
        return false;
    }
    if (!fs_->exists(namespacePath_) && !fs_->mkdir(namespacePath_)) {
        fail("CACHE NAMESPACE FAILED");
        return false;
    }
    recoverPair(namespacePath_ + "/sessions.jsonl");
    File manifest = fs_->open(namespacePath_ + "/manifest.json", FILE_READ);
    if (manifest) {
        JsonDocument doc;
        const bool valid = deserializeJson(doc, manifest) == DeserializationError::Ok &&
                           cacheSchemaCompatible(doc["schema"] | 0U);
        manifest.close();
        if (!valid) {
            fail("CACHE SCHEMA MISMATCH");
            return false;
        }
    } else if (!writeManifest()) {
        return false;
    }
    error_ = "";
    return true;
}

bool SdCache::writeManifest()
{
    File temp = fs_->open(namespacePath_ + "/manifest.tmp", FILE_WRITE);
    if (!temp) {
        fail("CACHE MANIFEST FAILED");
        return false;
    }
    temp.printf("{\"schema\":%u,\"format\":\"jsonl-crc32\"}\n",
                static_cast<unsigned>(kCacheSchemaVersion));
    temp.flush();
    temp.close();
    return replacePair(namespacePath_ + "/manifest.tmp",
                       namespacePath_ + "/manifest.json");
}

void SdCache::update()
{
    if (spoolBuffer_.length() &&
        millis() - spoolFlushMs_ >= kSpoolFlushIntervalMs) {
        flushAssistantSpool();
    }
}

void SdCache::fail(const String& message)
{
    error_ = message;
}

bool SdCache::recoverPair(const String& path)
{
    const String temp = path + ".tmp";
    const String backup = path + ".bak";
    if (!fs_->exists(path)) {
        if (fs_->exists(backup)) fs_->rename(backup, path);
        else if (fs_->exists(temp)) fs_->rename(temp, path);
    }
    if (fs_->exists(path)) {
        fs_->remove(temp);
        fs_->remove(backup);
    }
    return true;
}

bool SdCache::replacePair(const String& temp, const String& target)
{
    const String backup = target + ".bak";
    fs_->remove(backup);
    if (fs_->exists(target) && !fs_->rename(target, backup)) {
        fail("CACHE BACKUP FAILED");
        return false;
    }
    if (!fs_->rename(temp, target)) {
        if (fs_->exists(backup)) fs_->rename(backup, target);
        fail("CACHE COMMIT FAILED");
        return false;
    }
    fs_->remove(backup);
    return true;
}

bool SdCache::replaceHistoryFiles(const String& historyTemp,
                                  const String& historyTarget,
                                  const String& viewTemp,
                                  const String& viewTarget)
{
    // No cached verification result may survive a history/view mutation.
    verifiedSessionId_ = "";
    const String historyBackup = historyTarget + ".bak";
    const String viewBackup = viewTarget + ".bak";
    String transaction = historyTarget;
    if (transaction.endsWith(".jsonl")) transaction.remove(transaction.length() - 6);
    transaction += ".txn";
    fs_->remove(historyBackup);
    fs_->remove(viewBackup);
    fs_->remove(transaction);
    File marker = fs_->open(transaction, FILE_WRITE);
    if (!marker) return false;
    marker.print("history-pair-v1\n");
    marker.flush();
    marker.close();
    if (fs_->exists(historyTarget) &&
        !fs_->rename(historyTarget, historyBackup)) {
        fs_->remove(transaction);
        return false;
    }
    if (fs_->exists(viewTarget) && !fs_->rename(viewTarget, viewBackup)) {
        if (fs_->exists(historyBackup))
            fs_->rename(historyBackup, historyTarget);
        fs_->remove(transaction);
        return false;
    }
    const bool historyOk = fs_->rename(historyTemp, historyTarget);
    const bool viewOk = historyOk && fs_->rename(viewTemp, viewTarget);
    if (!historyOk || !viewOk) {
        fs_->remove(historyTarget);
        fs_->remove(viewTarget);
        if (fs_->exists(historyBackup))
            fs_->rename(historyBackup, historyTarget);
        if (fs_->exists(viewBackup)) fs_->rename(viewBackup, viewTarget);
        fs_->remove(transaction);
        fail("CACHE HISTORY COMMIT FAILED");
        return false;
    }
    // The pair now has new sizes and CRCs. Remove the old sidecar while the
    // transaction marker and backups still exist; a power loss before this
    // point rolls back, while a later metadata-write failure regenerates the
    // sidecar instead of quarantining valid new files against stale hashes.
    String metadata = historyTarget;
    if (metadata.endsWith(".jsonl")) {
        metadata.remove(metadata.length() - 6);
        metadata += ".meta";
        fs_->remove(metadata);
    }
    fs_->remove(transaction);
    fs_->remove(historyBackup);
    fs_->remove(viewBackup);
    return true;
}

void SdCache::recoverHistoryFiles(const String& sessionId)
{
    const String history = historyPath(sessionId);
    const String view = viewPath(sessionId);
    String transaction = sessionStem(sessionId) + ".txn";
    if (fs_->exists(transaction)) {
        const String historyBackup = history + ".bak";
        const String viewBackup = view + ".bak";
        if (fs_->exists(historyBackup)) {
            fs_->remove(history);
            fs_->rename(historyBackup, history);
        }
        if (fs_->exists(viewBackup)) {
            fs_->remove(view);
            fs_->rename(viewBackup, view);
        }
        fs_->remove(history + ".tmp");
        fs_->remove(view + ".tmp");
        fs_->remove(history + ".sync");
        fs_->remove(view + ".sync");
        fs_->remove(transaction);
    }
    recoverPair(history);
    recoverPair(view);
}

String SdCache::sessionStem(const String& sessionId) const
{
    return namespacePath_ + "/s-" + hexDigest(sessionId, 12);
}

String SdCache::historyPath(const String& sessionId) const
{
    return sessionStem(sessionId) + ".jsonl";
}

String SdCache::viewPath(const String& sessionId) const
{
    return sessionStem(sessionId) + ".view";
}

String SdCache::metaPath(const String& sessionId) const
{
    return sessionStem(sessionId) + ".meta";
}

bool SdCache::writeHistoryMeta(const String& sessionId)
{
    verifiedSessionId_ = "";
    File history = fs_->open(historyPath(sessionId), FILE_READ);
    File view = fs_->open(viewPath(sessionId), FILE_READ);
    if (!history || !view) {
        if (history) history.close();
        if (view) view.close();
        return false;
    }
    const std::size_t historyBytes = history.size();
    const std::size_t viewBytes = view.size();
    const std::uint32_t historyCrc = fileCrc32(history);
    const std::uint32_t viewCrc = fileCrc32(view);
    history.close();
    view.close();
    const String target = metaPath(sessionId);
    const String temp = target + ".tmp";
    fs_->remove(temp);
    File meta = fs_->open(temp, FILE_WRITE);
    if (!meta) return false;
    meta.printf("{\"schema\":%u,\"history_bytes\":%u,\"view_bytes\":%u,"
                "\"history_crc32\":%u,\"view_crc32\":%u}\n",
                static_cast<unsigned>(kCacheSchemaVersion),
                static_cast<unsigned>(historyBytes),
                static_cast<unsigned>(viewBytes),
                static_cast<unsigned>(historyCrc),
                static_cast<unsigned>(viewCrc));
    meta.flush();
    meta.close();
    const bool replaced = replacePair(temp, target);
    if (replaced) verifiedSessionId_ = sessionId;
    return replaced;
}

bool SdCache::verifyHistoryMeta(const String& sessionId)
{
    File meta = fs_->open(metaPath(sessionId), FILE_READ);
    if (!meta) return writeHistoryMeta(sessionId);
    JsonDocument document;
    const bool parsed = deserializeJson(document, meta) == DeserializationError::Ok;
    meta.close();
    File history = fs_->open(historyPath(sessionId), FILE_READ);
    File view = fs_->open(viewPath(sessionId), FILE_READ);
    const bool hasCrc = !document["history_crc32"].isNull() &&
                        !document["view_crc32"].isNull();
    const bool sizesValid = parsed && history && view &&
        cacheSchemaCompatible(document["schema"] | 0U) &&
        history.size() == static_cast<std::size_t>(document["history_bytes"] | 0U) &&
        view.size() == static_cast<std::size_t>(document["view_bytes"] | 0U);
    const bool valid = sizesValid && (!hasCrc ||
        (fileCrc32(history) == (document["history_crc32"] | 0U) &&
         fileCrc32(view) == (document["view_crc32"] | 0U)));
    if (history) history.close();
    if (view) view.close();
    if (valid && !hasCrc) return writeHistoryMeta(sessionId);
    if (valid) verifiedSessionId_ = sessionId;
    else quarantineHistory(sessionId);
    return valid;
}

void SdCache::quarantineHistory(const String& sessionId)
{
    if (verifiedSessionId_ == sessionId) verifiedSessionId_ = "";
    const String history = historyPath(sessionId);
    const String view = viewPath(sessionId);
    fs_->remove(history + ".bad");
    fs_->remove(view + ".bad");
    if (fs_->exists(history)) fs_->rename(history, history + ".bad");
    if (fs_->exists(view)) fs_->rename(view, view + ".bad");
    fs_->remove(metaPath(sessionId));
    fail("CACHE QUARANTINED");
}

bool SdCache::loadSessions(std::vector<CachedSession>& sessions,
                           std::size_t limit, std::size_t offset)
{
    if (!enabled_ || !fs_) return false;
    recoverPair(namespacePath_ + "/sessions.jsonl");
    File file = fs_->open(namespacePath_ + "/sessions.jsonl", FILE_READ);
    if (!file) return false;
    // Fill a local list so a page with no valid rows leaves the caller's
    // current page in place instead of emptying the visible list.
    std::vector<CachedSession> loaded;
    std::size_t skipped = 0;
    while (file.available() && loaded.size() < limit) {
        String line = file.readStringUntil('\n');
        if (!line.length()) continue;
        if (skipped < offset) { ++skipped; continue; }
        JsonDocument doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
        CachedSession session;
        session.id = doc["id"] | "";
        session.title = doc["title"] | "Untitled";
        session.preview = doc["preview"] | "";
        session.state = doc["state"] | "CACHED";
        if (session.id.length()) loaded.push_back(session);
    }
    file.close();
    if (loaded.empty()) return false;
    sessions.swap(loaded);
    return true;
}

std::size_t SdCache::sessionCount()
{
    if (!enabled_ || !fs_) return 0;
    File file = fs_->open(namespacePath_ + "/sessions.jsonl", FILE_READ);
    if (!file) return 0;
    std::size_t count = 0;
    while (file.available()) if (file.read() == '\n') ++count;
    file.close();
    return count;
}

bool SdCache::replaceSessions(JsonArrayConst sessions)
{
    if (!enabled_ || !fs_) return false;
    const String target = namespacePath_ + "/sessions.jsonl";
    const String temp = target + ".tmp";
    fs_->remove(temp);
    File file = fs_->open(temp, FILE_WRITE);
    if (!file) { fail("CACHE SESSION WRITE FAILED"); return false; }
    bool ok = true;
    for (JsonObjectConst item : sessions) {
        JsonDocument record;
        record["id"] = item["session_id"] | item["id"] | "";
        record["title"] = item["title"] | item["name"] | "Untitled";
        record["preview"] = item["preview"] | "";
        record["state"] = item["status"] | item["state"] | "";
        if (!serializeJson(record, file) || file.write('\n') != 1) {
            ok = false;
            break;
        }
    }
    file.flush();
    file.close();
    if (!ok) {
        fs_->remove(temp);
        fail("CACHE SESSION WRITE FAILED");
        return false;
    }
    return replacePair(temp, target);
}

bool SdCache::beginSessionsImport()
{
    abortSessionsImport();
    if (!enabled_ || !fs_) return false;
    sessionsImportTemp_ = namespacePath_ + "/sessions.sync";
    fs_->remove(sessionsImportTemp_);
    File file = fs_->open(sessionsImportTemp_, FILE_WRITE);
    if (!file) { sessionsImportTemp_ = ""; return false; }
    file.close();
    return true;
}

bool SdCache::appendSessionsImportPage(const String& responsePath,
                                       std::size_t& records)
{
    records = 0;
    if (!fs_ || !sessionsImportTemp_.length()) return false;
    File input = fs_->open(responsePath, FILE_READ);
    File output = fs_->open(sessionsImportTemp_, FILE_APPEND);
    if (!input || !output) {
        if (input) input.close();
        if (output) output.close();
        return false;
    }
    const char needle[] = "\"sessions\"";
    std::size_t matched = 0;
    bool found = false;
    while (input.available()) {
        const char value = static_cast<char>(input.read());
        if (value == needle[matched]) {
            if (++matched == sizeof(needle) - 1) { found = true; break; }
        } else matched = value == needle[0] ? 1 : 0;
    }
    while (found && input.available() && input.peek() != '[') input.read();
    bool ok = found && input.read() == '[';
    bool closed = false;
    while (ok && input.available()) {
        int next = input.peek();
        while (next == ',' || next == ' ' || next == '\r' || next == '\n' ||
               next == '\t') {
            input.read();
            next = input.peek();
        }
        if (next == ']') { input.read(); closed = true; break; }
        if (next != '{') { ok = false; break; }
        JsonDocument item;
        if (deserializeJson(item, input) != DeserializationError::Ok) {
            ok = false;
            break;
        }
        JsonDocument record;
        record["id"] = item["session_id"] | item["id"] | "";
        record["title"] = item["title"] | item["name"] | "Untitled";
        record["preview"] = item["preview"] | "";
        record["state"] = item["status"] | item["state"] | "";
        if (!serializeJson(record, output) || output.write('\n') != 1) {
            ok = false;
            break;
        }
        ++records;
    }
    input.close();
    output.flush();
    output.close();
    fs_->remove(responsePath);
    return ok && closed;
}

bool SdCache::commitSessionsImport()
{
    if (!sessionsImportTemp_.length()) return false;
    const String temp = sessionsImportTemp_;
    sessionsImportTemp_ = "";
    return replacePair(temp, namespacePath_ + "/sessions.jsonl");
}

void SdCache::abortSessionsImport()
{
    if (fs_ && sessionsImportTemp_.length()) fs_->remove(sessionsImportTemp_);
    sessionsImportTemp_ = "";
}

bool SdCache::writeRecord(File& jsonl, File& view, const char* role,
                          const String& text, const char* state,
                          const String& rowId)
{
    if (!jsonl || !view) return false;
    JsonDocument record;
    record["role"] = role;
    record["text"] = text;
    record["state"] = state;
    if (rowId.length()) record["id"] = rowId;
    record["crc32"] = cacheCrc32(
        reinterpret_cast<const std::uint8_t*>(text.c_str()), text.length());
    if (!serializeJson(record, jsonl) || jsonl.write('\n') != 1) return false;
    const String label = roleLabel(role);
    return view.print(label) == label.length() &&
           view.print(text) == text.length() && view.print("\n\n") == 2;
}

bool SdCache::replaceHistory(const String& sessionId, JsonArrayConst messages)
{
    if (!enabled_ || !fs_ || !sessionId.length()) return false;
    const String history = historyPath(sessionId);
    const String view = viewPath(sessionId);
    const String historyTemp = history + ".tmp";
    const String viewTemp = view + ".tmp";
    fs_->remove(historyTemp);
    fs_->remove(viewTemp);
    File jsonl = fs_->open(historyTemp, FILE_WRITE);
    File rendered = fs_->open(viewTemp, FILE_WRITE);
    if (!jsonl || !rendered) {
        if (jsonl) jsonl.close();
        if (rendered) rendered.close();
        fail("CACHE HISTORY WRITE FAILED");
        return false;
    }
    bool ok = true;
    for (JsonObjectConst item : messages) {
        const String role = item["role"] | item["type"] | "service";
        const String text = messageText(item);
        const String rowId = item["row_id"] | item["id"] | "";
        if (text.length() && !writeRecord(jsonl, rendered, role.c_str(), text,
                                          "confirmed", rowId)) {
            ok = false;
            break;
        }
    }
    jsonl.flush(); rendered.flush();
    jsonl.close(); rendered.close();
    if (!ok) {
        fs_->remove(historyTemp);
        fs_->remove(viewTemp);
        fail("CACHE HISTORY WRITE FAILED");
        return false;
    }
    if (!replaceHistoryFiles(historyTemp, history, viewTemp, view)) return false;
    if (!writeHistoryMeta(sessionId)) {
        fail("CACHE HISTORY META FAILED");
        return false;
    }
    enforceQuota();
    return true;
}

bool SdCache::replaceHistoryFromRest(const String& sessionId,
                                     const String& responsePath)
{
    if (!enabled_ || !fs_ || !sessionId.length()) return false;
    File input = fs_->open(responsePath, FILE_READ);
    if (!input) { fail("CACHE RESPONSE MISSING"); return false; }

    const String history = historyPath(sessionId);
    const String view = viewPath(sessionId);
    const String historyTemp = history + ".tmp";
    const String viewTemp = view + ".tmp";
    fs_->remove(historyTemp);
    fs_->remove(viewTemp);
    File jsonl = fs_->open(historyTemp, FILE_WRITE);
    File rendered = fs_->open(viewTemp, FILE_WRITE);
    std::size_t records = 0;
    const bool ok = jsonl && rendered &&
                    parseRestPage(input, jsonl, rendered, records);
    input.close();
    if (jsonl) { jsonl.flush(); jsonl.close(); }
    if (rendered) { rendered.flush(); rendered.close(); }
    fs_->remove(responsePath);
    if (!ok) {
        fs_->remove(historyTemp);
        fs_->remove(viewTemp);
        fail("CACHE RESPONSE PARSE FAILED");
        return false;
    }
    if (!replaceHistoryFiles(historyTemp, history, viewTemp, view)) return false;
    if (!writeHistoryMeta(sessionId)) {
        fail("CACHE HISTORY META FAILED");
        return false;
    }
    enforceQuota();
    return true;
}

bool SdCache::parseRestPage(File& input, File& jsonl, File& rendered,
                            std::size_t& records)
{
    // Locate the messages array without materializing the REST page. The
    // object parser below then owns only one row at a time.
    const char needle[] = "\"messages\"";
    std::size_t matched = 0;
    bool found = false;
    while (input.available()) {
        const char value = static_cast<char>(input.read());
        if (value == needle[matched]) {
            if (++matched == sizeof(needle) - 1) { found = true; break; }
        } else {
            matched = value == needle[0] ? 1 : 0;
        }
    }
    while (found && input.available() && input.peek() != '[') input.read();
    if (!found || input.read() != '[') {
        return false;
    }
    records = 0;
    bool closed = false;
    while (input.available()) {
        int next = input.peek();
        while (next == ',' || next == ' ' || next == '\r' || next == '\n' ||
               next == '\t') {
            input.read();
            next = input.peek();
        }
        if (next == ']') { input.read(); closed = true; break; }
        if (next != '{') return false;
        const std::size_t objectStart = input.position();
        JsonDocument filter;
        filter["role"] = true;
        filter["type"] = true;
        filter["row_id"] = true;
        filter["id"] = true;
        JsonDocument item;
        if (deserializeJson(item, input,
                            DeserializationOption::Filter(filter)) !=
            DeserializationError::Ok) {
            return false;
        }
        const std::size_t objectEnd = input.position();
        JsonObjectConst object = item.as<JsonObjectConst>();
        const String role = object["role"] | object["type"] | "service";
        const String rowId = object["row_id"] | object["id"] | "";
        if (!writeRestObject(input, objectStart, objectEnd, jsonl, rendered,
                             role, rowId)) {
            return false;
        }
        input.seek(objectEnd);
        ++records;
    }
    return closed;
}

bool SdCache::writeRestObject(File& input, std::size_t objectStart,
                              std::size_t objectEnd, File& jsonl, File& view,
                              const String& role, const String& rowId)
{
    const String label = roleLabel(role);
    if (view.print(label) != label.length()) return false;
    std::size_t segment = 0;
    auto scan = [&](bool rootOnly) -> bool {
        if (!input.seek(objectStart)) return false;
        int depth = 0;
        while (input.position() < objectEnd && input.available()) {
            const int value = input.read();
            if (value == '{') { ++depth; continue; }
            if (value == '}') { --depth; continue; }
            if (value != '"') continue;
            String key;
            if (!readJsonToken(input, objectEnd, key)) return false;
            while (input.position() < objectEnd && input.available() &&
                   (input.peek() == ' ' || input.peek() == '\r' ||
                    input.peek() == '\n' || input.peek() == '\t')) input.read();
            if (input.peek() != ':') continue;
            input.read();
            while (input.position() < objectEnd && input.available() &&
                   (input.peek() == ' ' || input.peek() == '\r' ||
                    input.peek() == '\n' || input.peek() == '\t')) input.read();
            const bool candidate = rootOnly
                                       ? depth == 1 &&
                                             (key == "text" || key == "content")
                                       : depth > 1 && key == "text";
            if (candidate && input.peek() == '"') {
                if (segment && view.write('\n') != 1) return false;
                input.read();
                if (!streamJsonText(input, objectEnd, jsonl, view, role, rowId,
                                    segment)) return false;
            }
        }
        return true;
    };
    if (!scan(true)) return false;
    if (!segment && !scan(false)) return false;
    return view.print("\n\n") == 2;
}

bool SdCache::streamJsonText(File& input, std::size_t objectEnd, File& jsonl,
                             File& view, const String& role,
                             const String& rowId, std::size_t& segment)
{
    String chunk;
    chunk.reserve(520);
    auto flush = [&]() -> bool {
        if (!chunk.length()) return true;
        JsonDocument record;
        record["role"] = role;
        record["text"] = chunk;
        record["state"] = "confirmed";
        if (rowId.length()) record["id"] = rowId;
        record["segment"] = segment++;
        record["crc32"] = cacheCrc32(
            reinterpret_cast<const std::uint8_t*>(chunk.c_str()),
            chunk.length());
        const bool written = serializeJson(record, jsonl) > 0 &&
                             jsonl.write('\n') == 1 &&
                             view.print(chunk) == chunk.length();
        if (!written) return false;
        chunk = "";
        return true;
    };
    while (input.position() < objectEnd && input.available()) {
        int value = input.read();
        if (value == '"') return flush();
        if (value == '\\') {
            value = input.read();
            if (value == 'n') value = '\n';
            else if (value == 'r') value = '\r';
            else if (value == 't') value = '\t';
            else if (value == 'b') value = '\b';
            else if (value == 'f') value = '\f';
            else if (value == 'u') {
                std::uint16_t first = 0;
                if (!readHex4(input, first)) return false;
                std::uint32_t codepoint = first;
                if (first >= 0xd800 && first <= 0xdbff) {
                    const std::size_t saved = input.position();
                    if (input.read() == '\\' && input.read() == 'u') {
                        std::uint16_t second = 0;
                        if (readHex4(input, second) && second >= 0xdc00 &&
                            second <= 0xdfff) {
                            codepoint = 0x10000U +
                                ((first - 0xd800U) << 10U) +
                                (second - 0xdc00U);
                        } else {
                            input.seek(saved);
                            codepoint = 0xfffd;
                        }
                    } else {
                        input.seek(saved);
                        codepoint = 0xfffd;
                    }
                }
                appendUtf8(chunk, codepoint);
                if (chunk.length() >= 512 && !flush()) return false;
                continue;
            }
        }
        if (value < 0) return false;
        chunk += static_cast<char>(value);
        if (chunk.length() >= 512 && !flush()) return false;
    }
    return false;
}

bool SdCache::beginHistoryImport(const String& sessionId)
{
    abortHistoryImport();
    if (!enabled_ || !fs_ || !sessionId.length()) return false;
    importHistoryPath_ = historyPath(sessionId);
    importViewPath_ = viewPath(sessionId);
    importHistoryTemp_ = importHistoryPath_ + ".sync";
    importViewTemp_ = importViewPath_ + ".sync";
    importSessionId_ = sessionId;
    fs_->remove(importHistoryTemp_);
    fs_->remove(importViewTemp_);
    File history = fs_->open(importHistoryTemp_, FILE_WRITE);
    File view = fs_->open(importViewTemp_, FILE_WRITE);
    const bool ok = history && view;
    if (history) history.close();
    if (view) view.close();
    if (!ok) {
        abortHistoryImport();
        fail("CACHE IMPORT OPEN FAILED");
    }
    return ok;
}

bool SdCache::appendHistoryImportPage(const String& responsePath,
                                      std::size_t& records)
{
    records = 0;
    if (!fs_ || !importHistoryTemp_.length()) return false;
    File input = fs_->open(responsePath, FILE_READ);
    File history = fs_->open(importHistoryTemp_, FILE_APPEND);
    File view = fs_->open(importViewTemp_, FILE_APPEND);
    const bool ok = input && history && view &&
                    parseRestPage(input, history, view, records);
    if (input) input.close();
    if (history) { history.flush(); history.close(); }
    if (view) { view.flush(); view.close(); }
    fs_->remove(responsePath);
    if (!ok) fail("CACHE IMPORT PARSE FAILED");
    return ok;
}

bool SdCache::commitHistoryImport()
{
    if (!fs_ || !importHistoryTemp_.length()) return false;
    const bool committed = replaceHistoryFiles(
        importHistoryTemp_, importHistoryPath_, importViewTemp_, importViewPath_);
    if (committed) {
        if (!writeHistoryMeta(importSessionId_)) {
            fail("CACHE HISTORY META FAILED");
            return false;
        }
        importHistoryPath_ = "";
        importViewPath_ = "";
        importHistoryTemp_ = "";
        importViewTemp_ = "";
        importSessionId_ = "";
        enforceQuota();
    }
    return committed;
}

void SdCache::abortHistoryImport()
{
    if (fs_) {
        if (importHistoryTemp_.length()) fs_->remove(importHistoryTemp_);
        if (importViewTemp_.length()) fs_->remove(importViewTemp_);
    }
    importHistoryPath_ = "";
    importViewPath_ = "";
    importHistoryTemp_ = "";
    importViewTemp_ = "";
    importSessionId_ = "";
}

bool SdCache::loadLatestAssistant(File& view, String& output)
{
    output = "";
    const char marker[] = "\n\nHERMES: ";
    constexpr std::size_t markerLength = sizeof(marker) - 1;
    constexpr std::size_t overlapBytes = markerLength - 1;
    constexpr std::size_t blockBytes = 512;
    char block[blockBytes + overlapBytes];
    char overlap[overlapBytes];
    std::size_t overlapLength = 0;
    std::size_t cursor = view.size();
    std::size_t textStart = static_cast<std::size_t>(-1);

    while (cursor && textStart == static_cast<std::size_t>(-1)) {
        const std::size_t start = cursor > blockBytes ? cursor - blockBytes : 0;
        const std::size_t requested = cursor - start;
        if (!view.seek(start)) return false;
        const std::size_t count = view.read(
            reinterpret_cast<std::uint8_t*>(block), requested);
        memcpy(block + count, overlap, overlapLength);
        const std::size_t combined = count + overlapLength;
        for (int index = static_cast<int>(combined - min(combined, markerLength));
             index >= 0; --index) {
            if (static_cast<std::size_t>(index) + markerLength <= combined &&
                memcmp(block + index, marker, markerLength) == 0) {
                textStart = start + index + markerLength;
                break;
            }
        }
        if (start == 0 && textStart == static_cast<std::size_t>(-1) &&
            combined >= 8 && memcmp(block, "HERMES: ", 8) == 0) {
            textStart = 8;
        }
        overlapLength = min(overlapBytes, count);
        memcpy(overlap, block, overlapLength);
        cursor = start;
    }
    if (textStart == static_cast<std::size_t>(-1) || !view.seek(textStart))
        return false;

    output.reserve(kLatestAssistantBytes + 4);
    while (view.available() && output.length() < kLatestAssistantBytes + 4) {
        output += static_cast<char>(view.read());
        if (output.endsWith("\n\nYOU: ")) {
            output.remove(output.length() - 7);
            break;
        }
    }
    if (output.length() > kLatestAssistantBytes) {
        std::size_t safe = kLatestAssistantBytes;
        while (safe && safe < output.length() &&
               (static_cast<unsigned char>(output[safe]) & 0xC0U) == 0x80U)
            --safe;
        output.remove(safe);
    }
    output.trim();
    return output.length() > 0;
}

bool SdCache::loadTimelineWindow(const String& sessionId,
                                 std::size_t requestedStart,
                                 String& timeline, String& lastAssistant,
                                 std::size_t& windowStart,
                                 std::size_t& windowEnd,
                                 std::size_t& totalBytes)
{
    if (!enabled_ || !fs_ || !sessionId.length()) return false;
    recoverHistoryFiles(sessionId);
    recoverAssistantSpool(sessionId);
    if (verifiedSessionId_ != sessionId && !verifyHistoryMeta(sessionId))
        return false;
    File file = fs_->open(viewPath(sessionId), FILE_READ);
    if (!file) return false;
    totalBytes = file.size();
    const bool touch = requestedStart == static_cast<std::size_t>(-1);
    if (touch) {
        requestedStart = totalBytes > kCacheRamWindowBytes
                             ? totalBytes - kCacheRamWindowBytes : 0;
    }
    requestedStart = min(requestedStart, totalBytes);
    if (requestedStart) file.seek(requestedStart);
    timeline = "";
    timeline.reserve(kCacheRamWindowBytes + 1);
    std::size_t remaining = min(kCacheRamWindowBytes,
                                totalBytes - requestedStart);
    while (remaining-- && file.available()) {
        timeline += static_cast<char>(file.read());
    }
    if (touch) loadLatestAssistant(file, lastAssistant);
    file.close();
    windowStart = requestedStart;
    if (requestedStart) {
        const std::size_t safe = utf8SafeStart(timeline.c_str(),
                                                timeline.length(), 0);
        if (safe) { timeline.remove(0, safe); windowStart += safe; }
        const int boundary = timeline.indexOf('\n');
        // A very large single message may have no newline in this window.
        // Keep its bounded text slice instead of returning an empty screen.
        if (boundary >= 0 && boundary < 256) {
            timeline.remove(0, boundary + 1);
            windowStart += boundary + 1;
        }
    }
    if (windowStart + timeline.length() < totalBytes) {
        const int boundary = timeline.lastIndexOf('\n');
        if (boundary >= 0 &&
            static_cast<std::size_t>(boundary) > timeline.length() - 256) {
            timeline.remove(boundary + 1);
        }
    }
    windowEnd = windowStart + timeline.length();
    if (touch) touchSession(sessionId);
    return timeline.length() > 0;
}

bool SdCache::appendMessage(const String& sessionId, const char* role,
                            const String& text, const char* state,
                            const String& rowId)
{
    if (!enabled_ || !fs_ || !sessionId.length() || !text.length()) return false;
    if (verifiedSessionId_ == sessionId) verifiedSessionId_ = "";
    File jsonl = fs_->open(historyPath(sessionId), FILE_APPEND);
    File view = fs_->open(viewPath(sessionId), FILE_APPEND);
    if (!writeRecord(jsonl, view, role, text, state, rowId)) {
        if (jsonl) jsonl.close();
        if (view) view.close();
        fail("CACHE APPEND FAILED");
        return false;
    }
    jsonl.flush(); view.flush();
    jsonl.close(); view.close();
    return writeHistoryMeta(sessionId);
}

bool SdCache::updateMessageState(const String& sessionId, const String& rowId,
                                 const char* state)
{
    if (!enabled_ || !fs_ || !sessionId.length() || !rowId.length()) return false;
    const String history = historyPath(sessionId);
    const String view = viewPath(sessionId);
    const String historyTemp = history + ".tmp";
    const String viewTemp = view + ".tmp";
    File source = fs_->open(history, FILE_READ);
    File output = fs_->open(historyTemp, FILE_WRITE);
    if (!source || !output) {
        if (source) source.close();
        if (output) output.close();
        return false;
    }
    bool changed = false;
    bool outputOk = true;
    while (source.available() && outputOk) {
        String line = source.readStringUntil('\n');
        JsonDocument record;
        if (!changed && deserializeJson(record, line) == DeserializationError::Ok &&
            String(record["id"] | "") == rowId) {
            record["state"] = state;
            outputOk = serializeJson(record, output) > 0;
            changed = true;
        } else {
            outputOk = output.print(line) == line.length();
        }
        if (outputOk) outputOk = output.write('\n') == 1;
    }
    source.close();
    output.flush();
    output.close();
    if (!changed || !outputOk) {
        fs_->remove(historyTemp);
        if (!outputOk) fail("CACHE STATE WRITE FAILED");
        return false;
    }
    File viewSource = fs_->open(view, FILE_READ);
    File viewOutput = fs_->open(viewTemp, FILE_WRITE);
    std::uint8_t buffer[256];
    while (viewSource && viewOutput && viewSource.available()) {
        const std::size_t count = viewSource.read(buffer, sizeof(buffer));
        if (count && viewOutput.write(buffer, count) != count) break;
    }
    const bool copied = viewSource && viewOutput &&
                        viewOutput.size() == viewSource.size();
    if (viewSource) viewSource.close();
    if (viewOutput) { viewOutput.flush(); viewOutput.close(); }
    if (!copied || !replaceHistoryFiles(historyTemp, history, viewTemp, view)) {
        fs_->remove(historyTemp);
        fs_->remove(viewTemp);
        return false;
    }
    return writeHistoryMeta(sessionId);
}

bool SdCache::beginAssistantSpool(const String& sessionId)
{
    abandonAssistantSpool();
    if (!enabled_ || !fs_ || !sessionId.length()) return false;
    spoolSessionId_ = sessionId;
    spoolPath_ = sessionStem(sessionId) + ".spool";
    recoverAssistantSpool(sessionId);
    fs_->remove(spoolPath_);
    File file = fs_->open(spoolPath_, FILE_WRITE);
    if (!file) { fail("CACHE SPOOL FAILED"); return false; }
    file.close();
    spoolFlushMs_ = millis();
    return true;
}

bool SdCache::recoverAssistantSpool(const String& sessionId)
{
    const String spool = sessionStem(sessionId) + ".spool";
    if (!fs_->exists(spool)) return true;
    const String history = historyPath(sessionId);
    const String view = viewPath(sessionId);
    const String historyTemp = history + ".tmp";
    const String viewTemp = view + ".tmp";
    fs_->remove(historyTemp);
    fs_->remove(viewTemp);
    File historyOutput = fs_->open(historyTemp, FILE_WRITE);
    File viewOutput = fs_->open(viewTemp, FILE_WRITE);
    if (!historyOutput || !viewOutput) return false;
    std::uint8_t copyBuffer[256];
    File historySource = fs_->open(history, FILE_READ);
    while (historySource && historySource.available()) {
        const std::size_t count = historySource.read(copyBuffer, sizeof(copyBuffer));
        if (historyOutput.write(copyBuffer, count) != count) return false;
    }
    if (historySource) historySource.close();
    File viewSource = fs_->open(view, FILE_READ);
    while (viewSource && viewSource.available()) {
        const std::size_t count = viewSource.read(copyBuffer, sizeof(copyBuffer));
        if (viewOutput.write(copyBuffer, count) != count) return false;
    }
    if (viewSource) viewSource.close();

    File interrupted = fs_->open(spool, FILE_READ);
    if (!interrupted) return false;
    viewOutput.print("HERMES: ");
    std::size_t segment = 0;
    while (interrupted.available()) {
        String chunk;
        chunk.reserve(512);
        while (interrupted.available() && chunk.length() < 512)
            chunk += static_cast<char>(interrupted.read());
        JsonDocument record;
        record["role"] = "assistant";
        record["text"] = chunk;
        record["state"] = "interrupted";
        record["segment"] = segment++;
        record["crc32"] = cacheCrc32(
            reinterpret_cast<const std::uint8_t*>(chunk.c_str()), chunk.length());
        serializeJson(record, historyOutput);
        historyOutput.write('\n');
        viewOutput.print(chunk);
    }
    interrupted.close();
    viewOutput.print("\n[INTERRUPTED]\n\n");
    historyOutput.flush();
    viewOutput.flush();
    historyOutput.close();
    viewOutput.close();
    if (!replaceHistoryFiles(historyTemp, history, viewTemp, view)) return false;
    fs_->remove(spool);
    return writeHistoryMeta(sessionId);
}

bool SdCache::appendAssistantDelta(const String& delta)
{
    if (!spoolPath_.length() || !delta.length()) return false;
    if (!spoolFile_) spoolFile_ = fs_->open(spoolPath_, FILE_APPEND);
    if (!spoolFile_) { fail("CACHE SPOOL WRITE FAILED"); return false; }
    spoolBuffer_ += delta;
    if (spoolBuffer_.length() < kSpoolFlushBytes) return true;
    return flushAssistantSpool();
}

bool SdCache::flushAssistantSpool()
{
    if (!spoolBuffer_.length()) return true;
    if (!spoolFile_) spoolFile_ = fs_->open(spoolPath_, FILE_APPEND);
    if (!spoolFile_) { fail("CACHE SPOOL WRITE FAILED"); return false; }
    const bool ok = spoolFile_.print(spoolBuffer_) == spoolBuffer_.length();
    if (ok) {
        spoolFile_.flush();
        spoolBuffer_ = "";
        spoolFlushMs_ = millis();
    } else {
        fail("CACHE SPOOL WRITE FAILED");
    }
    return ok;
}

bool SdCache::finalizeAssistantSpool(const String& finalText)
{
    if (!spoolSessionId_.length()) return false;
    if (spoolFile_) {
        if (!flushAssistantSpool()) return false;
        spoolFile_.flush();
        spoolFile_.close();
    }
    String text = finalText;
    if (!text.length()) {
        File file = fs_->open(spoolPath_, FILE_READ);
        if (file) { text = file.readString(); file.close(); }
    }
    const bool ok = text.length() && appendMessage(spoolSessionId_, "assistant",
                                                    text, "confirmed");
    abandonAssistantSpool();
    return ok;
}

void SdCache::touchSession(const String& sessionId)
{
    // Replacing the tiny metadata sidecar updates the cache access time
    // without rewriting the potentially large history or rendered view.
    const String target = metaPath(sessionId);
    File source = fs_->open(target, FILE_READ);
    if (!source) return;
    const String metadata = source.readString();
    source.close();
    const String temp = target + ".touch";
    fs_->remove(temp);
    File output = fs_->open(temp, FILE_WRITE);
    if (!output) return;
    const bool written = output.print(metadata) == metadata.length();
    output.flush();
    output.close();
    if (written) replacePair(temp, target);
    else fs_->remove(temp);
}

void SdCache::abandonAssistantSpool()
{
    if (spoolFile_) spoolFile_.close();
    spoolBuffer_ = "";
    if (fs_ && spoolPath_.length()) fs_->remove(spoolPath_);
    spoolSessionId_ = "";
    spoolPath_ = "";
}

std::uint64_t SdCache::usageBytes()
{
    if (!fs_ || !namespacePath_.length()) return 0;
    std::uint64_t total = 0;
    File directory = fs_->open(namespacePath_);
    if (!directory || !directory.isDirectory()) return 0;
    File entry = directory.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) total += entry.size();
        entry.close();
        entry = directory.openNextFile();
    }
    directory.close();
    return total;
}

bool SdCache::enforceQuota()
{
    if (!fs_ || !enabled_) return false;
    const std::uint64_t quota = static_cast<std::uint64_t>(quotaMb_) *
                                1024ULL * 1024ULL;
    std::uint64_t total = usageBytes();
    while (total > quota) {
        File directory = fs_->open(namespacePath_);
        if (!directory || !directory.isDirectory()) return false;
        String oldest;
        std::uint32_t oldestTime = UINT32_MAX;
        File entry = directory.openNextFile();
        while (entry) {
            const String path = entry.path();
            if (!entry.isDirectory() && path.endsWith(".meta") &&
                (!protectedStem_.length() || path != protectedStem_ + ".meta")) {
                const std::uint32_t written = entry.getLastWrite();
                if (!oldest.length() || written < oldestTime) {
                    oldest = path;
                    oldestTime = written;
                }
            }
            entry.close();
            entry = directory.openNextFile();
        }
        directory.close();
        if (!oldest.length()) return false;
        String stem = oldest;
        stem.remove(stem.length() - 5);
        fs_->remove(stem + ".jsonl");
        fs_->remove(stem + ".view");
        fs_->remove(stem + ".spool");
        fs_->remove(oldest);
        const std::uint64_t next = usageBytes();
        if (next >= total) return false;
        total = next;
    }
    return true;
}

bool SdCache::clear()
{
    if (!fs_ || !namespacePath_.length()) return false;
    verifiedSessionId_ = "";
    abandonAssistantSpool();
    abortHistoryImport();
    while (true) {
        File directory = fs_->open(namespacePath_);
        if (!directory || !directory.isDirectory()) return false;
        String victim;
        File entry = directory.openNextFile();
        while (entry) {
            const String name = entry.path();
            const bool removable = !entry.isDirectory();
            entry.close();
            if (removable) { victim = name; break; }
            entry = directory.openNextFile();
        }
        directory.close();
        if (!victim.length()) break;
        if (!fs_->remove(victim)) return false;
    }
    // Clearing also discards an incompatible manifest so a schema mismatch
    // is recoverable on-device; the current schema is written back.
    return writeManifest();
}

}  // namespace hermes_terminal
