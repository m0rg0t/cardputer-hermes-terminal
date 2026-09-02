#include "hermes_terminal/hermes_client.h"

#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include "hermes_terminal/ws_frame_rules.h"

namespace hermes_terminal {
namespace {

constexpr std::size_t kMaxInboundFrame = 16 * 1024;
constexpr std::size_t kMaxOutboundFrame = 8 * 1024;
constexpr unsigned long kClientPingIntervalMs = 30000;
constexpr unsigned long kPasswordLoginRetryMs = 60000;
constexpr unsigned long kRestIdleTimeoutMs = 15000;
constexpr unsigned long kWriteStallMs = 5000;
constexpr std::size_t kRestWorkBytes = 1024;

bool writeAll(WiFiClientSecure& client, const std::uint8_t* data,
              std::size_t length, bool (*cancelCheck)() = nullptr)
{
    std::size_t offset = 0;
    unsigned long lastProgress = millis();
    while (offset < length) {
        if (cancelCheck && cancelCheck()) return false;
        const std::size_t written = client.write(data + offset,
                                                 length - offset);
        if (written) {
            offset += written;
            lastProgress = millis();
        } else if (!client.connected() ||
                   millis() - lastProgress >= kWriteStallMs) {
            return false;
        } else {
            delay(1);
        }
        yield();
    }
    return true;
}

String base64(const std::uint8_t* input, std::size_t length)
{
    std::size_t required = 0;
    mbedtls_base64_encode(nullptr, 0, &required, input, length);
    String result;
    if (!result.reserve(required + 1)) {
        return "";
    }
    std::unique_ptr<unsigned char[]> output(new unsigned char[required + 1]);
    std::size_t written = 0;
    if (mbedtls_base64_encode(output.get(), required + 1, &written,
                              input, length) != 0) {
        return "";
    }
    output[written] = '\0';
    result = reinterpret_cast<const char*>(output.get());
    return result;
}

String websocketAccept(const String& key)
{
    String material = key;
    material += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::uint8_t digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(material.c_str()),
                 material.length(), digest);
    return base64(digest, sizeof(digest));
}

String urlEncode(const String& value)
{
    static const char hex[] = "0123456789ABCDEF";
    String result;
    result.reserve(value.length() * 3);
    for (std::size_t index = 0; index < value.length(); ++index) {
        const std::uint8_t c = static_cast<std::uint8_t>(value[index]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0x0F];
        }
    }
    return result;
}

String httpHeaderValue(const String& headers, const char* name)
{
    String lower = headers;
    lower.toLowerCase();
    String needle = "\r\n";
    needle += name;
    needle += ':';
    needle.toLowerCase();
    const int at = lower.indexOf(needle);
    if (at < 0) return "";
    const int start = at + needle.length();
    const int end = headers.indexOf("\r\n", start);
    String value = headers.substring(start, end < 0 ? headers.length() : end);
    value.trim();
    return value;
}

bool headerHasToken(const String& value, const char* expected)
{
    String remaining = value;
    remaining.toLowerCase();
    String token = expected;
    token.toLowerCase();
    int start = 0;
    while (start <= static_cast<int>(remaining.length())) {
        int end = remaining.indexOf(',', start);
        if (end < 0) end = remaining.length();
        String candidate = remaining.substring(start, end);
        candidate.trim();
        if (candidate == token) return true;
        start = end + 1;
    }
    return false;
}

String decodeChunkedBody(const String& raw)
{
    String output;
    int offset = 0;
    while (offset < static_cast<int>(raw.length())) {
        const int end = raw.indexOf("\r\n", offset);
        if (end < 0) return "";
        const std::size_t size =
            strtoul(raw.substring(offset, end).c_str(), nullptr, 16);
        if (!size) return output;
        offset = end + 2;
        if (offset + static_cast<int>(size) + 2 >
            static_cast<int>(raw.length())) return "";
        output += raw.substring(offset, offset + size);
        offset += size + 2;
    }
    return "";
}

void replaceCookie(String& cookies, const String& pair)
{
    const int equals = pair.indexOf('=');
    if (equals <= 0) return;
    const String name = pair.substring(0, equals);
    String result;
    int start = 0;
    while (start < static_cast<int>(cookies.length())) {
        int end = cookies.indexOf(';', start);
        if (end < 0) end = cookies.length();
        String existing = cookies.substring(start, end);
        existing.trim();
        if (!existing.startsWith(name + "=")) {
            if (result.length()) result += "; ";
            result += existing;
        }
        start = end + 1;
    }
    if (pair.length() > static_cast<std::size_t>(equals + 1)) {
        if (result.length()) result += "; ";
        result += pair;
    }
    cookies = result;
}

String mergedResponseCookies(const String& current, const String& headers)
{
    String result = current;
    String lower = headers;
    lower.toLowerCase();
    int start = 0;
    while (start < static_cast<int>(headers.length())) {
        int end = headers.indexOf("\r\n", start);
        if (end < 0) end = headers.length();
        if (lower.substring(start, min(end, start + 11)) == "set-cookie:") {
            String pair = headers.substring(start + 11, end);
            pair.trim();
            const int semicolon = pair.indexOf(';');
            if (semicolon >= 0) pair = pair.substring(0, semicolon);
            replaceCookie(result, pair);
        }
        start = end + 2;
    }
    return result;
}

}  // namespace

bool HermesClient::begin(const Config& config, const String& caPem,
                         HermesClientListener& listener)
{
    config_ = config;
    caPem_ = caPem;
    listener_ = &listener;
    if (!parseBaseUrl()) {
        diagnostic_ = "INVALID HERMES URL";
        return false;
    }
    state_ = State::kWaitingWifi;
    nextConnectMs_ = 0;
    return true;
}

bool HermesClient::parseBaseUrl()
{
    if (!config_.baseUrl.startsWith("https://")) {
        return false;
    }
    String authority = config_.baseUrl.substring(8);
    const int slash = authority.indexOf('/');
    if (slash >= 0) {
        basePath_ = authority.substring(slash);
        authority = authority.substring(0, slash);
    } else {
        basePath_ = "";
    }
    const int colon = authority.lastIndexOf(':');
    if (colon > 0 && authority.indexOf(']') < colon) {
        host_ = authority.substring(0, colon);
        port_ = static_cast<std::uint16_t>(authority.substring(colon + 1).toInt());
    } else {
        host_ = authority;
        port_ = 443;
    }
    return host_.length() > 0 && port_ > 0;
}

void HermesClient::update()
{
    if (state_ == State::kRestConnecting ||
        state_ == State::kRestHeaders || state_ == State::kRestBody) {
        updateRestDownload();
        return;
    }
    if (state_ == State::kRestDone) return;
    if (WiFi.status() != WL_CONNECTED) {
        if (state_ == State::kConnected || state_ == State::kConnecting) {
            fail("WIFI LOST");
        }
        state_ = State::kWaitingWifi;
        return;
    }
    if (state_ != State::kConnected) {
        if (millis() >= nextConnectMs_) {
            state_ = State::kConnecting;
            if (!connectNow()) {
                state_ = State::kBackoff;
                reconnectDelayMs_ = nextReconnectDelayMs(reconnectDelayMs_);
                nextConnectMs_ = millis() + reconnectDelayMs_;
            }
        }
        return;
    }
    if (!socket_.connected()) {
        fail("SOCKET CLOSED");
        return;
    }
    // Consume at most one frame per pass. A busy Hermes session can produce a
    // continuous stream of resume/history progress events; draining the whole
    // socket here would starve the Cardputer keyboard and make ESC appear dead.
    if (socket_.available() > 0 && state_ == State::kConnected) {
        if (!readFrame()) return;
    }
    if (state_ == State::kConnected &&
        millis() - lastPingMs_ >= kClientPingIntervalMs) {
        const std::uint32_t nonce = esp_random();
        if (!sendFrame(0x9, reinterpret_cast<const std::uint8_t*>(&nonce),
                       sizeof(nonce))) {
            fail("WS PING FAILED");
            return;
        }
        lastPingMs_ = millis();
    }
    if (state_ == State::kConnected &&
        linkTimedOut(millis(), lastReceiveMs_, kClientPingIntervalMs)) {
        fail("WS TIMEOUT");
    }
}

bool HermesClient::beginRestDownload(const String& path,
                                     fs::FS& destination,
                                     const String& outputPath)
{
    if (restDownloadActive() || restResultReady_ ||
        WiFi.status() != WL_CONNECTED || !path.startsWith("/")) {
        return false;
    }
    restFs_ = &destination;
    restRequestPath_ = path;
    restOutputPath_ = outputPath;
    restHeaders_ = "";
    restChunkLine_ = "";
    restStatus_ = 0;
    restBytes_ = 0;
    restContentLength_ = 0;
    restChunkRemaining_ = 0;
    restChunked_ = false;
    restNeedChunkSize_ = false;
    restChunkCrlf_ = 0;
    restResult_ = RestDownloadResult();
    destination.remove(outputPath);
    restFile_ = destination.open(outputPath, FILE_WRITE);
    if (!restFile_) {
        restFs_ = nullptr;
        diagnostic_ = "CACHE DOWNLOAD OPEN FAILED";
        return false;
    }
    socket_.stop();
    diagnostic_ = "SYNC CONNECTING";
    state_ = State::kRestConnecting;
    restLastDataMs_ = millis();
    return true;
}

bool HermesClient::restDownloadActive() const
{
    return state_ == State::kRestConnecting ||
           state_ == State::kRestHeaders || state_ == State::kRestBody;
}

void HermesClient::cancelRestDownload()
{
    if (restDownloadActive()) finishRestDownload("SYNC CANCELLED", true);
}

bool HermesClient::takeRestDownloadResult(RestDownloadResult& result)
{
    if (!restResultReady_) return false;
    result = restResult_;
    restResultReady_ = false;
    restResult_ = RestDownloadResult();
    state_ = State::kWaitingWifi;
    nextConnectMs_ = 0;
    return true;
}

void HermesClient::finishRestDownload(const String& error, bool cancelled)
{
    if (restFile_) {
        restFile_.flush();
        restFile_.close();
    }
    socket_.stop();
    restResult_.status = restStatus_;
    restResult_.bytes = restBytes_;
    restResult_.path = restOutputPath_;
    restResult_.error = error;
    restResult_.cancelled = cancelled;
    if ((cancelled || error.length()) && restFs_ && restOutputPath_.length()) {
        restFs_->remove(restOutputPath_);
    }
    diagnostic_ = error.length() ? error : "SYNC COMPLETE";
    restResultReady_ = true;
    state_ = State::kRestDone;
}

bool HermesClient::writeRestByte(std::uint8_t value)
{
    if (!restFile_ || restFile_.write(&value, 1) != 1) {
        finishRestDownload("CACHE DOWNLOAD WRITE FAILED");
        return false;
    }
    ++restBytes_;
    return true;
}

void HermesClient::updateRestDownload()
{
    if (WiFi.status() != WL_CONNECTED) {
        finishRestDownload("SYNC WIFI LOST");
        return;
    }
    if (millis() - restLastDataMs_ > kRestIdleTimeoutMs) {
        finishRestDownload("SYNC TIMEOUT");
        return;
    }
    if (state_ == State::kRestConnecting) {
        socket_.setCACert(caPem_.c_str());
        socket_.setTimeout(1);
        socket_.setHandshakeTimeout(2);
        if (!socket_.connect(host_.c_str(), port_, 1500)) {
            finishRestDownload("SYNC TLS FAILED");
            return;
        }
        socket_.setTimeout(1);
        socket_.print("GET " + basePath_ + restRequestPath_ + " HTTP/1.1\r\n");
        socket_.print("Host: " + host_ + ":" + String(port_) + "\r\n");
        socket_.print("Accept: application/json\r\n");
        socket_.print("Accept-Encoding: identity\r\nConnection: close\r\n");
        if (config_.sessionCookie.length()) {
            socket_.print("Cookie: " + config_.sessionCookie + "\r\n");
        } else if (config_.sessionToken.length()) {
            socket_.print("X-Hermes-Session-Token: " + config_.sessionToken +
                          "\r\n");
        }
        socket_.print("\r\n");
        state_ = State::kRestHeaders;
        diagnostic_ = "SYNC HEADERS";
        restLastDataMs_ = millis();
        return;
    }

    std::size_t budget = kRestWorkBytes;
    if (state_ == State::kRestHeaders) {
        while (budget-- && socket_.available()) {
            restHeaders_ += static_cast<char>(socket_.read());
            restLastDataMs_ = millis();
            if (restHeaders_.length() > 4096) {
                finishRestDownload("SYNC HEADERS TOO LARGE");
                return;
            }
            if (restHeaders_.endsWith("\r\n\r\n")) {
                const int firstSpace = restHeaders_.indexOf(' ');
                restStatus_ = firstSpace >= 0
                                  ? restHeaders_.substring(firstSpace + 1,
                                                           firstSpace + 4).toInt()
                                  : 0;
                restContentLength_ = static_cast<std::size_t>(
                    httpHeaderValue(restHeaders_, "content-length").toInt());
                restChunked_ = headerHasToken(
                    httpHeaderValue(restHeaders_, "transfer-encoding"),
                    "chunked");
                restNeedChunkSize_ = restChunked_;
                state_ = State::kRestBody;
                diagnostic_ = "SYNCING CACHE";
                break;
            }
        }
        if (state_ == State::kRestHeaders && !socket_.connected() &&
            !socket_.available()) {
            finishRestDownload("SYNC HEADER CLOSED");
        }
        return;
    }

    budget = kRestWorkBytes;
    while (state_ == State::kRestBody && budget-- && socket_.available()) {
        const int input = socket_.read();
        if (input < 0) break;
        restLastDataMs_ = millis();
        const std::uint8_t byte = static_cast<std::uint8_t>(input);
        if (!restChunked_) {
            if (!writeRestByte(byte)) return;
            if (restContentLength_ && restBytes_ >= restContentLength_) {
                finishRestDownload();
                return;
            }
            continue;
        }
        if (restNeedChunkSize_) {
            if (byte == '\n') {
                restChunkLine_.trim();
                const int extension = restChunkLine_.indexOf(';');
                if (extension >= 0) restChunkLine_.remove(extension);
                char* end = nullptr;
                restChunkRemaining_ = strtoul(restChunkLine_.c_str(), &end, 16);
                if (!restChunkLine_.length() || !end || *end != '\0') {
                    finishRestDownload("SYNC CHUNK INVALID");
                    return;
                }
                restChunkLine_ = "";
                restNeedChunkSize_ = false;
                if (!restChunkRemaining_) {
                    finishRestDownload();
                    return;
                }
            } else if (byte != '\r') {
                if (restChunkLine_.length() >= 16) {
                    finishRestDownload("SYNC CHUNK INVALID");
                    return;
                }
                restChunkLine_ += static_cast<char>(byte);
            }
            continue;
        }
        if (restChunkCrlf_) {
            const std::uint8_t expected = restChunkCrlf_ == 2 ? '\r' : '\n';
            if (byte != expected) {
                finishRestDownload("SYNC CHUNK INVALID");
                return;
            }
            --restChunkCrlf_;
            if (!restChunkCrlf_) restNeedChunkSize_ = true;
            continue;
        }
        if (!writeRestByte(byte)) return;
        if (--restChunkRemaining_ == 0) restChunkCrlf_ = 2;
    }
    if (state_ == State::kRestBody && !socket_.connected() &&
        !socket_.available()) {
        if (!restChunked_ && (!restContentLength_ ||
            restBytes_ == restContentLength_)) {
            finishRestDownload();
        } else {
            finishRestDownload("SYNC BODY TRUNCATED");
        }
    }
}

bool HermesClient::connectNow()
{
    // /api/status is public and lets the device explain a token/cookie
    // mismatch before spending a connection attempt on the wrong auth path.
    // A failed probe is non-fatal: older gateways may not expose every
    // capability field, so the configured credential remains the source of
    // truth when the status endpoint is unavailable.
    // Resolve first so an unreachable gateway reads as "DNS FAILED host"
    // (wrong network, or a .local name without mDNS) rather than a generic
    // TLS failure; a routable host that does not answer keeps the TLS text.
    {
        IPAddress resolved;
        if (!WiFi.hostByName(host_.c_str(), resolved) ||
            resolved == IPAddress()) {
            diagnostic_ = "DNS FAILED " + host_;
            return false;
        }
    }
    if (!statusProbeAttempted_ || millis() >= nextStatusProbeMs_) {
        probeGatewayStatus();
    }
    if (!authConfigured()) {
        diagnostic_ = "AUTH CREDENTIAL REQUIRED";
        return false;
    }
    if (gatewayAuthMode_ == "cookie" && config_.sessionToken.length()) {
        diagnostic_ = "COOKIE REQUIRED BY GATEWAY";
        return false;
    }
    if (gatewayAuthMode_ == "token" &&
        (config_.sessionCookie.length() || config_.loginUsername.length())) {
        diagnostic_ = "TOKEN MODE REQUIRED BY GATEWAY";
        return false;
    }

    String authName = "token";
    String authValue = config_.sessionToken;
    if (!config_.sessionCookie.length() && config_.loginUsername.length() &&
        !passwordLogin()) {
        return false;
    }
    if (config_.sessionCookie.length() > 0) {
        authName = "ticket";
        if (!obtainTicket(authValue)) {
            const bool sessionRejected = diagnostic_.startsWith("AUTH EXPIRED") ||
                                         diagnostic_.startsWith("AUTH FORBIDDEN");
            if (!config_.loginUsername.length() || !sessionRejected ||
                !passwordLogin() || !obtainTicket(authValue)) {
                return false;
            }
        }
    }
    if (!openWebSocket(authName, authValue)) {
        return false;
    }
    state_ = State::kConnected;
    diagnostic_ = "CONNECTED";
    lastReceiveMs_ = millis();
    lastPingMs_ = millis();
    if (listener_) listener_->onHermesConnected();
    return true;
}

bool HermesClient::httpRequest(const char* method, const String& path,
                               const String& requestBody,
                               const String& cookie, int& status,
                               String& headers, String& response,
                               CancelCheck cancelCheck)
{
    const auto cancelled = [&]() {
        if (!cancelCheck || !cancelCheck()) return false;
        diagnostic_ = "AUTH CANCELLED";
        return true;
    };
    if (cancelled()) return false;
    WiFiClientSecure client;
    client.setCACert(caPem_.c_str());
    client.setTimeout(15);
    client.setHandshakeTimeout(5);
    if (!client.connect(host_.c_str(), port_, 5000)) {
        cancelled();
        return false;
    }
    // connect(..., timeoutMs) also fixes the TLS write stall budget for this
    // connection (the core does not re-read setTimeout after connect), so
    // kWriteStallMs matches that 5 s budget rather than the 15 s read wait.
    if (cancelled()) {
        client.stop();
        return false;
    }
    client.print(String(method) + " " + basePath_ + path + " HTTP/1.1\r\n");
    client.print("Host: " + host_ + ":" + String(port_) + "\r\n");
    client.print("Accept-Encoding: identity\r\nConnection: close\r\n");
    if (cookie.length()) client.print("Cookie: " + cookie + "\r\n");
    if (requestBody.length()) {
        client.print("Content-Type: application/json\r\nContent-Length: " +
                     String(requestBody.length()) + "\r\n");
    }
    client.print("\r\n");
    if (requestBody.length() &&
        !writeAll(client,
                  reinterpret_cast<const std::uint8_t*>(requestBody.c_str()),
                  requestBody.length(), cancelCheck)) {
        cancelled();
        client.stop();
        return false;
    }

    headers = "";
    response = "";
    unsigned long lastData = millis();
    while (headers.length() <= 4096 &&
           headers.indexOf("\r\n\r\n") < 0 &&
           millis() - lastData < 15000) {
        if (cancelled()) {
            client.stop();
            return false;
        }
        while (client.available()) {
            headers += static_cast<char>(client.read());
            lastData = millis();
            if (headers.length() > 4096) break;
        }
        if (!client.connected() && !client.available()) break;
        delay(1);
    }
    const int headerEnd = headers.indexOf("\r\n\r\n");
    if (headerEnd < 0 || headerEnd > 4096) {
        client.stop();
        return false;
    }
    response = headers.substring(headerEnd + 4);
    headers.remove(headerEnd + 2);
    const int firstSpace = headers.indexOf(' ');
    status = firstSpace >= 0
                 ? headers.substring(firstSpace + 1, firstSpace + 4).toInt()
                 : 0;
    const int expected = httpHeaderValue(headers, "content-length").toInt();
    lastData = millis();
    while (response.length() < 16384 &&
           (!expected || static_cast<int>(response.length()) < expected) &&
           millis() - lastData < 15000) {
        if (cancelled()) {
            client.stop();
            return false;
        }
        while (client.available() && response.length() < 16384) {
            response += static_cast<char>(client.read());
            lastData = millis();
        }
        if (!client.connected() && !client.available()) break;
        delay(1);
    }
    client.stop();
    if (cancelled()) return false;
    if (headerHasToken(httpHeaderValue(headers, "transfer-encoding"),
                       "chunked")) {
        response = decodeChunkedBody(response);
    }
    return status > 0 && (!expected ||
                          static_cast<int>(response.length()) >= expected);
}

bool HermesClient::passwordLogin(CancelCheck cancelCheck)
{
    if (!config_.loginUsername.length() || !config_.loginPassword.length()) {
        diagnostic_ = "LOGIN CREDENTIAL REQUIRED";
        return false;
    }
    if (nextPasswordLoginMs_ && millis() < nextPasswordLoginMs_) {
        diagnostic_ = "LOGIN RETRY WAIT";
        return false;
    }
    nextPasswordLoginMs_ = millis() + kPasswordLoginRetryMs;

    JsonDocument request;
    request["provider"] = "basic";
    request["username"] = config_.loginUsername;
    request["password"] = config_.loginPassword;
    request["next"] = "/";
    String body;
    serializeJson(request, body);
    int status = 0;
    String headers;
    String response;
    if (!httpRequest("POST", "/auth/password-login", body, "", status,
                     headers, response, cancelCheck)) {
        if (cancelCheck && cancelCheck()) {
            diagnostic_ = "AUTH CANCELLED";
            nextPasswordLoginMs_ = 0;
        } else {
            diagnostic_ = "LOGIN HTTP FAILED";
        }
        return false;
    }
    const String cookie = mergedResponseCookies("", headers);

    if (status != 200) {
        if (status == 401) {
            diagnostic_ = "LOGIN INVALID (401)";
        } else if (status == 429) {
            diagnostic_ = "LOGIN RATE LIMITED";
        } else {
            diagnostic_ = "LOGIN HTTP " + String(status);
        }
        return false;
    }
    if (cookie.length() < 16 || cookie.indexOf('=') <= 0) {
        diagnostic_ = "LOGIN COOKIE MISSING";
        return false;
    }

    config_.sessionCookie = cookie;
    nextPasswordLoginMs_ = 0;
    if (listener_) listener_->onHermesAuthCookieUpdated(cookie);
    diagnostic_ = "LOGIN OK";
    return true;
}

bool HermesClient::obtainTicket(String& ticket, CancelCheck cancelCheck)
{
    int status = 0;
    String headers;
    String body;
    if (!httpRequest("POST", "/api/auth/ws-ticket", "{}",
                     config_.sessionCookie, status, headers, body,
                     cancelCheck)) {
        if (cancelCheck && cancelCheck()) {
            diagnostic_ = "AUTH CANCELLED";
        } else {
            diagnostic_ = "TICKET HTTP FAILED";
        }
        return false;
    }
    const String updatedCookie =
        mergedResponseCookies(config_.sessionCookie, headers);
    if (updatedCookie.length() && updatedCookie != config_.sessionCookie) {
        config_.sessionCookie = updatedCookie;
        if (listener_) listener_->onHermesAuthCookieUpdated(updatedCookie);
    }
    if (status != 200) {
        if (status == 401) {
            diagnostic_ = "AUTH EXPIRED (TICKET 401)";
        } else if (status == 403) {
            diagnostic_ = "AUTH FORBIDDEN (TICKET 403)";
        } else {
            diagnostic_ = "TICKET HTTP " + String(status);
        }
        return false;
    }
    JsonDocument document;
    if (deserializeJson(document, body)) {
        diagnostic_ = "TICKET INVALID JSON";
        return false;
    }
    ticket = document["ticket"] | "";
    if (ticket.length() < 16) {
        diagnostic_ = "TICKET MISSING";
        return false;
    }
    return true;
}

bool HermesClient::refreshAuthentication(CancelCheck cancelCheck)
{
    if (cancelCheck && cancelCheck()) {
        diagnostic_ = "AUTH CANCELLED";
        return false;
    }
    if (!config_.sessionCookie.length()) {
        return config_.loginUsername.length() ? passwordLogin(cancelCheck)
                                              : true;
    }
    String unusedTicket;
    if (obtainTicket(unusedTicket, cancelCheck)) return true;
    if (diagnostic_ == "AUTH CANCELLED") return false;
    const bool sessionRejected = diagnostic_.startsWith("AUTH EXPIRED") ||
                                 diagnostic_.startsWith("AUTH FORBIDDEN");
    return config_.loginUsername.length() && sessionRejected &&
           passwordLogin(cancelCheck) &&
           obtainTicket(unusedTicket, cancelCheck);
}

bool HermesClient::openWebSocket(const String& authName,
                                 const String& authValue)
{
    socket_.stop();
    socket_.setCACert(caPem_.c_str());
    // The REST download path tunes handshake/socket timeouts on this same
    // socket and stop() preserves them, so set both explicitly here. The
    // connect timeout also fixes the TLS write stall budget for this link.
    socket_.setHandshakeTimeout(5);
    if (!socket_.connect(host_.c_str(), port_, 5000)) {
        diagnostic_ = "TLS CONNECT FAILED";
        return false;
    }

    std::uint8_t nonce[16];
    for (std::uint8_t& byte : nonce) {
        byte = static_cast<std::uint8_t>(esp_random());
    }
    const String key = base64(nonce, sizeof(nonce));
    String path = basePath_ + "/api/ws?" + authName + "=" + urlEncode(authValue);
    socket_.print("GET " + path + " HTTP/1.1\r\n");
    socket_.print("Host: " + host_ + ":" + String(port_) + "\r\n");
    socket_.print("Upgrade: websocket\r\nConnection: Upgrade\r\n");
    socket_.print("Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n");
    String origin = "https://" + host_;
    if (port_ != 443) origin += ":" + String(port_);
    socket_.print("Origin: " + origin + "\r\n\r\n");

    String headers;
    const unsigned long deadline = millis() + 10000;
    while (millis() < deadline && headers.indexOf("\r\n\r\n") < 0) {
        while (socket_.available() && headers.indexOf("\r\n\r\n") < 0) {
            headers += static_cast<char>(socket_.read());
            if (headers.length() > 4096) {
                diagnostic_ = "WS HEADERS TOO LARGE";
                socket_.stop();
                return false;
            }
        }
        delay(1);
    }
    if (!headers.startsWith("HTTP/1.1 101") &&
        !headers.startsWith("HTTP/1.0 101")) {
        const int firstSpace = headers.indexOf(' ');
        const int secondSpace = firstSpace >= 0
                                    ? headers.indexOf(' ', firstSpace + 1)
                                    : -1;
        const int responseStatus = firstSpace >= 0 && secondSpace > firstSpace
                                        ? headers.substring(firstSpace + 1,
                                                            secondSpace).toInt()
                                        : 0;
        if (responseStatus == 401) {
            diagnostic_ = "AUTH EXPIRED (WS 401)";
        } else if (responseStatus == 403) {
            diagnostic_ = "AUTH FORBIDDEN (WS 403)";
        } else {
            diagnostic_ = "WS UPGRADE HTTP " + String(responseStatus);
        }
        socket_.stop();
        return false;
    }
    if (!headerHasToken(httpHeaderValue(headers, "upgrade"), "websocket") ||
        !headerHasToken(httpHeaderValue(headers, "connection"), "upgrade")) {
        diagnostic_ = "WS UPGRADE HEADERS INVALID";
        socket_.stop();
        return false;
    }
    const String receivedAccept =
        httpHeaderValue(headers, "sec-websocket-accept");
    if (!receivedAccept.length()) {
        diagnostic_ = "WS ACCEPT MISSING";
        socket_.stop();
        return false;
    }
    if (receivedAccept != websocketAccept(key)) {
        diagnostic_ = "WS ACCEPT INVALID";
        socket_.stop();
        return false;
    }
    return true;
}

std::uint32_t HermesClient::request(const char* method,
                                    JsonObjectConst params)
{
    if (!connected()) return 0;
    JsonDocument document;
    const std::uint32_t id = nextRequestId_++;
    document["jsonrpc"] = "2.0";
    document["id"] = id;
    document["method"] = method;
    document["params"].set(params);
    String text;
    serializeJson(document, text);
    if (!sendText(text)) return 0;
    return id;
}

std::uint32_t HermesClient::request(const char* method)
{
    JsonDocument params;
    params.to<JsonObject>();
    return request(method, params.as<JsonObjectConst>());
}

bool HermesClient::sendText(const String& text)
{
    if (!connected() || text.length() > kMaxOutboundFrame) return false;
    return sendFrame(0x1,
                     reinterpret_cast<const std::uint8_t*>(text.c_str()),
                     text.length());
}

bool HermesClient::sendFrame(std::uint8_t opcode, const std::uint8_t* data,
                             std::size_t length)
{
    if (!connected() || length > kMaxOutboundFrame ||
        (opcode >= 0x8 && length > 125)) return false;
    std::uint8_t header[8];
    std::size_t headerLength = 0;
    header[headerLength++] = 0x80 | (opcode & 0x0F);
    if (length <= 125) {
        header[headerLength++] = 0x80 | static_cast<std::uint8_t>(length);
    } else {
        header[headerLength++] = 0x80 | 126;
        header[headerLength++] = static_cast<std::uint8_t>(length >> 8);
        header[headerLength++] = static_cast<std::uint8_t>(length);
    }
    std::uint8_t mask[4];
    const std::uint32_t random = esp_random();
    memcpy(mask, &random, sizeof(mask));
    memcpy(header + headerLength, mask, sizeof(mask));
    headerLength += sizeof(mask);
    if (!writeAll(socket_, header, headerLength)) {
        fail("WS WRITE HEADER");
        return false;
    }
    std::uint8_t chunk[256];
    std::size_t offset = 0;
    while (offset < length) {
        const std::size_t count = min(sizeof(chunk), length - offset);
        for (std::size_t i = 0; i < count; ++i) {
            chunk[i] = data[offset + i] ^ mask[(offset + i) & 3U];
        }
        if (!writeAll(socket_, chunk, count)) {
            fail("WS WRITE BODY");
            return false;
        }
        offset += count;
    }
    return true;
}

bool HermesClient::readExact(std::uint8_t* data, std::size_t length,
                             unsigned long timeoutMs)
{
    std::size_t offset = 0;
    const unsigned long deadline = millis() + timeoutMs;
    while (offset < length && millis() < deadline) {
        const int count = socket_.read(data + offset, length - offset);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else delay(1);
    }
    return offset == length;
}

bool HermesClient::readFrame()
{
    std::uint8_t first[2];
    if (!readExact(first, sizeof(first), 1000)) {
        fail("WS FRAME HEADER");
        return false;
    }
    const std::uint8_t opcode = first[0] & 0x0F;
    std::uint64_t length = first[1] & 0x7F;
    if (length == 126) {
        std::uint8_t extended[2];
        if (!readExact(extended, 2, 1000)) {
            fail("WS FRAME HEADER");
            return false;
        }
        length = (static_cast<std::uint16_t>(extended[0]) << 8) | extended[1];
    } else if (length == 127) {
        std::uint8_t extended[8];
        if (!readExact(extended, 8, 1000)) {
            fail("WS FRAME HEADER");
            return false;
        }
        length = 0;
        for (std::uint8_t byte : extended) length = (length << 8) | byte;
    }
    const ServerFrameError frameError =
        validateServerFrame(first[0], first[1], length);
    if (frameError != ServerFrameError::kNone) {
        const char* reason = "WS UNSUPPORTED FRAME";
        if (frameError == ServerFrameError::kMasked) {
            reason = "WS MASKED SERVER FRAME";
        } else if (frameError == ServerFrameError::kControlTooLarge) {
            reason = "WS CONTROL FRAME TOO LARGE";
        } else if (frameError == ServerFrameError::kInvalidCloseLength) {
            reason = "WS INVALID CLOSE FRAME";
        } else if (frameError == ServerFrameError::kUnknownOpcode) {
            reason = "WS UNKNOWN OPCODE";
        }
        fail(reason);
        return false;
    }
    if (length > kMaxInboundFrame) {
        // Dropping one oversized event is cheaper than a reconnect that
        // loses the event anyway: session.resume does not replay it.
        std::uint8_t sink[256];
        std::uint64_t remaining = length;
        while (remaining) {
            const std::size_t count = static_cast<std::size_t>(
                remaining < sizeof(sink) ? remaining : sizeof(sink));
            if (!readExact(sink, count, 2000)) {
                fail("WS FRAME BODY");
                return false;
            }
            remaining -= count;
        }
        lastReceiveMs_ = millis();
        diagnostic_ = "WS FRAME DROPPED";
        return true;
    }
    String payload;
    if (!payload.reserve(static_cast<std::size_t>(length) + 1)) {
        fail("WS OUT OF MEMORY");
        return false;
    }
    std::uint8_t chunk[256];
    std::size_t offset = 0;
    while (offset < length) {
        const std::size_t count = min(sizeof(chunk),
                                      static_cast<std::size_t>(length - offset));
        if (!readExact(chunk, count, 2000)) {
            fail("WS FRAME BODY");
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            payload += static_cast<char>(chunk[index]);
        }
        offset += count;
    }
    lastReceiveMs_ = millis();
    if (opcode == 0x1) {
        handleText(payload);
    } else if (opcode == 0x8) {
        sendFrame(0x8,
                  reinterpret_cast<const std::uint8_t*>(payload.c_str()),
                  min<std::size_t>(payload.length(), 125));
        fail("WS CLOSED BY SERVER");
        return false;
    } else if (opcode == 0x9) {
        if (!sendFrame(0xA,
                       reinterpret_cast<const std::uint8_t*>(payload.c_str()),
                       payload.length())) {
            fail("WS PONG FAILED");
            return false;
        }
    }
    return true;
}

void HermesClient::handleText(const String& text)
{
    int start = 0;
    while (start < static_cast<int>(text.length())) {
        int end = text.indexOf('\n', start);
        if (end < 0) end = text.length();
        String line = text.substring(start, end);
        line.trim();
        if (line.length() > 0) {
            JsonDocument document;
            if (!deserializeJson(document, line) && listener_) {
                listener_->onHermesMessage(document);
            }
        }
        start = end + 1;
    }
}

void HermesClient::fail(const String& reason)
{
    const bool notify = state_ == State::kConnected;
    socket_.stop();
    diagnostic_ = reason;
    state_ = State::kBackoff;
    // A link that was up resets the backoff; repeated failures keep doubling.
    if (notify) reconnectDelayMs_ = 0;
    reconnectDelayMs_ = nextReconnectDelayMs(reconnectDelayMs_);
    nextConnectMs_ = millis() + reconnectDelayMs_;
    if (notify && listener_) listener_->onHermesDisconnected(reason);
}

bool HermesClient::connected()
{
    return state_ == State::kConnected && socket_.connected();
}

bool HermesClient::connectionFailed() const
{
    return state_ == State::kBackoff ||
           (state_ == State::kIdle && diagnostic_ != "NOT STARTED" &&
            diagnostic_ != "CONNECTING");
}

String HermesClient::diagnostic() const
{
    return diagnostic_;
}

String HermesClient::authMode() const
{
    if (config_.loginUsername.length() && config_.loginPassword.length()) {
        return "password-cookie";
    }
    if (config_.sessionCookie.length()) return "cookie";
    if (config_.sessionToken.length()) return "token";
    return "unconfigured";
}

String HermesClient::gatewayAuthMode() const
{
    return gatewayAuthMode_;
}

String HermesClient::gatewayAuthFlows() const
{
    return gatewayAuthFlows_;
}

bool HermesClient::authConfigured() const
{
    return config_.sessionCookie.length() >= 16 ||
           config_.sessionToken.length() >= 16 ||
           (config_.loginUsername.length() && config_.loginPassword.length());
}

void HermesClient::setSessionCookie(const String& cookie)
{
    config_.sessionCookie = cookie;
    config_.sessionToken = "";
    diagnostic_ = "COOKIE SAVED - RECONNECTING";
}

bool HermesClient::probeGatewayStatus()
{
    statusProbeAttempted_ = true;
    nextStatusProbeMs_ = millis() + 300000UL;

    int status = 0;
    String headers;
    String body;
    if (!httpRequest("GET", "/api/status", "", "", status, headers,
                     body)) {
        gatewayAuthMode_ = "unavailable";
        gatewayAuthFlows_ = "";
        diagnostic_ = "STATUS HTTP FAILED";
        return false;
    }
    if (status != 200) {
        gatewayAuthRequired_ = false;
        gatewayAuthMode_ = "unavailable";
        gatewayAuthFlows_ = "";
        diagnostic_ = "STATUS HTTP " + String(status);
        return false;
    }

    JsonDocument document;
    if (deserializeJson(document, body)) {
        gatewayAuthRequired_ = false;
        gatewayAuthMode_ = "unavailable";
        gatewayAuthFlows_ = "";
        diagnostic_ = "STATUS INVALID JSON";
        return false;
    }

    gatewayAuthRequired_ = document["auth_required"] | false;
    gatewayAuthMode_ = gatewayAuthRequired_ ? "cookie" : "token";
    gatewayAuthFlows_ = "";
    JsonArrayConst flows = document["auth_flows"].as<JsonArrayConst>();
    for (JsonVariantConst flow : flows) {
        const String name = flow.as<String>();
        if (!name.length()) continue;
        if (gatewayAuthFlows_.length()) gatewayAuthFlows_ += ",";
        gatewayAuthFlows_ += name;
    }
    diagnostic_ = gatewayAuthRequired_ ? "GATEWAY COOKIE" : "GATEWAY TOKEN";
    return true;
}

void HermesClient::disconnect()
{
    if (restDownloadActive()) {
        cancelRestDownload();
        return;
    }
    socket_.stop();
    state_ = State::kWaitingWifi;
    diagnostic_ = "CONNECTING";
    nextConnectMs_ = 0;
    nextPasswordLoginMs_ = 0;
}

}  // namespace hermes_terminal
