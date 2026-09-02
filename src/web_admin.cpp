#include "hermes_terminal/web_admin.h"

#include <WiFi.h>

#include "admin_page_gz.h"

namespace hermes_terminal {

bool WebAdmin::begin(const Config& config, WebAdminListener& listener)
{
    if (!config.webAdmin) return true;
    if (config.webAdminToken.length() < 16) return false;
    listener_ = &listener;
    username_ = config.webAdminUsername.length() ? config.webAdminUsername : "hermes";
    token_ = config.webAdminToken;
    const char* headers[] = {"X-Hermes-Admin"};
    server_.collectHeaders(headers, 1);

    server_.on("/", HTTP_GET, [this]() {
        if (!authorize()) return;
        server_.sendHeader("Cache-Control", "no-store");
        server_.sendHeader("Content-Encoding", "gzip");
        server_.send_P(200, "text/html; charset=utf-8",
                       reinterpret_cast<PGM_P>(kAdminPageGzip), kAdminPageGzipSize);
    });
    server_.on("/api/status", HTTP_GET, [this]() {
        if (!authorize()) return;
        JsonDocument document;
        listener_->writeWebStatus(document.to<JsonObject>());
        sendJson(document);
    });
    server_.on("/api/auth/cookie", HTTP_POST, [this]() {
        if (!authorize()) return;
        if (server_.header("X-Hermes-Admin") != "1") {
            server_.send(403, "application/json", "{\"error\":\"admin header required\"}");
            return;
        }
        JsonDocument input;
        JsonDocument output;
        const String body = server_.arg("plain");
        if (body.length() > 4608) {
            server_.send(413, "application/json", "{\"error\":\"cookie request too large\"}");
            return;
        }
        const auto error = deserializeJson(input, body);
        const String cookie = input["cookie"] | "";
        const bool ok = !error && listener_->updateWebAuthCookie(cookie);
        output["ok"] = ok;
        if (!ok) output["error"] = "cookie rejected or could not be saved";
        sendJson(output, ok ? 202 : 409);
    });
    server_.on("/api/prompt", HTTP_POST, [this]() {
        if (!authorize()) return;
        if (server_.header("X-Hermes-Admin") != "1") {
            server_.send(403, "application/json", "{\"error\":\"admin header required\"}");
            return;
        }
        JsonDocument input;
        JsonDocument output;
        const String body = server_.arg("plain");
        if (body.length() > 4608) {
            server_.send(413, "application/json", "{\"error\":\"prompt too large\"}");
            return;
        }
        const auto error = deserializeJson(input, body);
        const String text = input["text"] | "";
        const bool ok = !error && text.length() && text.length() <= 4000 &&
                        listener_->submitWebPrompt(text);
        output["ok"] = ok;
        sendJson(output, ok ? 202 : 409);
    });
    server_.on("/api/interrupt", HTTP_POST, [this]() {
        if (!authorize()) return;
        if (server_.header("X-Hermes-Admin") != "1") {
            server_.send(403, "application/json", "{\"error\":\"admin header required\"}");
            return;
        }
        JsonDocument output;
        const bool ok = listener_->interruptWebSession();
        output["ok"] = ok;
        sendJson(output, ok ? 202 : 409);
    });
    server_.onNotFound([this]() {
        if (!authorize()) return;
        server_.send(404, "application/json", "{\"error\":\"not found\"}");
    });
    server_.begin();
    enabled_ = true;
    return true;
}

void WebAdmin::update()
{
    if (enabled_) server_.handleClient();
}

bool WebAdmin::authorize()
{
    if (server_.authenticate(username_.c_str(), token_.c_str())) return true;
    server_.requestAuthentication(BASIC_AUTH, "Hermes Terminal");
    return false;
}

void WebAdmin::sendJson(JsonDocument& document, int status)
{
    String body;
    serializeJson(document, body);
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(status, "application/json", body);
}

}  // namespace hermes_terminal
