#include "hermes_terminal/hermes_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include "hermes_terminal/ws_frame_rules.h"

namespace hermes_terminal {
namespace {

constexpr std::size_t kMaxInboundFrame = 16 * 1024;
constexpr std::size_t kMaxOutboundFrame = 8 * 1024;
constexpr unsigned long kReconnectDelayMs = 5000;
constexpr unsigned long kClientPingIntervalMs = 30000;
constexpr unsigned long kPasswordLoginRetryMs = 60000;

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

void seedCookieJar(const String& header, const String& host, CookieJar& jar)
{
    // HTTPClient's Set-Cookie parser assigns a host-only cookie to the
    // registrable-looking suffix after the second-to-last dot. Mirror that
    // behavior so a rotated cookie replaces the bootstrap value instead of
    // creating a duplicate name under a different internal domain.
    String cookieDomain = host;
    const int lastDot = host.lastIndexOf('.');
    const int secondLastDot = lastDot > 0 ? host.lastIndexOf('.', lastDot - 1)
                                          : -1;
    if (secondLastDot >= 0) cookieDomain = host.substring(secondLastDot + 1);
    int start = 0;
    while (start < static_cast<int>(header.length())) {
        int end = header.indexOf(';', start);
        if (end < 0) end = header.length();
        String pair = header.substring(start, end);
        pair.trim();
        const int equals = pair.indexOf('=');
        if (equals > 0) {
            Cookie cookie{};
            cookie.host = host;
            cookie.domain = cookieDomain;
            cookie.path = "/";
            cookie.secure = true;
            cookie.name = pair.substring(0, equals);
            cookie.value = pair.substring(equals + 1);
            cookie.name.trim();
            if (cookie.name.length()) jar.push_back(cookie);
        }
        start = end + 1;
    }
}

String serializeCookieJar(const CookieJar& jar)
{
    String result;
    for (const Cookie& cookie : jar) {
        if (!cookie.name.length() || !cookie.value.length()) continue;
        if (result.length()) result += "; ";
        result += cookie.name + "=" + cookie.value;
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
                nextConnectMs_ = millis() + kReconnectDelayMs;
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
}

bool HermesClient::connectNow()
{
    // /api/status is public and lets the device explain a token/cookie
    // mismatch before spending a connection attempt on the wrong auth path.
    // A failed probe is non-fatal: older gateways may not expose every
    // capability field, so the configured credential remains the source of
    // truth when the status endpoint is unavailable.
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

bool HermesClient::passwordLogin()
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

    WiFiClientSecure client;
    client.setCACert(caPem_.c_str());
    HTTPClient http;
    const String url = config_.baseUrl + "/auth/password-login";
    if (!http.begin(client, url)) {
        diagnostic_ = "LOGIN URL FAILED";
        return false;
    }
    CookieJar cookieJar;
    http.setCookieJar(&cookieJar);
    http.setTimeout(15000);
    http.addHeader("Content-Type", "application/json");
    JsonDocument request;
    request["provider"] = "basic";
    request["username"] = config_.loginUsername;
    request["password"] = config_.loginPassword;
    request["next"] = "/";
    String body;
    serializeJson(request, body);
    const int status = http.POST(body);
    http.getString();
    const String cookie = serializeCookieJar(cookieJar);
    http.end();

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

bool HermesClient::obtainTicket(String& ticket)
{
    WiFiClientSecure client;
    client.setCACert(caPem_.c_str());
    HTTPClient http;
    const String url = config_.baseUrl + "/api/auth/ws-ticket";
    if (!http.begin(client, url)) {
        diagnostic_ = "TICKET URL FAILED";
        return false;
    }
    CookieJar cookieJar;
    seedCookieJar(config_.sessionCookie, host_, cookieJar);
    http.setCookieJar(&cookieJar);
    http.setTimeout(15000);
    http.addHeader("Content-Type", "application/json");
    const int status = http.POST("{}");
    const String body = http.getString();
    const String updatedCookie = serializeCookieJar(cookieJar);
    http.end();
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

bool HermesClient::refreshAuthentication()
{
    if (!config_.sessionCookie.length()) {
        return config_.loginUsername.length() ? passwordLogin() : true;
    }
    String unusedTicket;
    if (obtainTicket(unusedTicket)) return true;
    const bool sessionRejected = diagnostic_.startsWith("AUTH EXPIRED") ||
                                 diagnostic_.startsWith("AUTH FORBIDDEN");
    return config_.loginUsername.length() && sessionRejected &&
           passwordLogin() && obtainTicket(unusedTicket);
}

bool HermesClient::openWebSocket(const String& authName,
                                 const String& authValue)
{
    socket_.stop();
    socket_.setCACert(caPem_.c_str());
    socket_.setTimeout(2);
    if (!socket_.connect(host_.c_str(), port_)) {
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
    if (socket_.write(header, headerLength) != headerLength) {
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
        if (socket_.write(chunk, count) != count) {
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
        if (!readExact(extended, 2, 1000)) return false;
        length = (static_cast<std::uint16_t>(extended[0]) << 8) | extended[1];
    } else if (length == 127) {
        std::uint8_t extended[8];
        if (!readExact(extended, 8, 1000)) return false;
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
        fail("WS FRAME TOO LARGE");
        return false;
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
    nextConnectMs_ = millis() + kReconnectDelayMs;
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

    WiFiClientSecure client;
    client.setCACert(caPem_.c_str());
    HTTPClient http;
    const String url = config_.baseUrl + "/api/status";
    if (!http.begin(client, url)) {
        gatewayAuthMode_ = "unavailable";
        gatewayAuthFlows_ = "";
        diagnostic_ = "STATUS URL FAILED";
        return false;
    }
    http.setTimeout(10000);
    const int status = http.GET();
    const String body = http.getString();
    http.end();
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
    socket_.stop();
    state_ = State::kWaitingWifi;
    diagnostic_ = "CONNECTING";
    nextConnectMs_ = 0;
    nextPasswordLoginMs_ = 0;
}

}  // namespace hermes_terminal
