#include "hermes_terminal/web_admin.h"

#include <WiFi.h>

#include "admin_page_gz.h"

namespace hermes_terminal {

namespace {
constexpr int kMaxPostBody = 4608;
}

// The stock handlers buffer a whole POST body before any callback runs, so
// an unauthenticated LAN client could exhaust the heap. A raw handler sees
// the headers first and can decline to read the body at all.
class WebAdmin::PostHandler : public RequestHandler {
public:
    explicit PostHandler(WebAdmin& admin) : admin_(admin) {}
    bool canHandle(HTTPMethod method, String) override
    {
        return method == HTTP_POST;
    }
    bool canRaw(String) override { return true; }
    void raw(WebServer&, String, HTTPRaw& raw) override
    {
        admin_.receiveBody(raw);
    }
    bool handle(WebServer&, HTTPMethod method, String uri) override
    {
        if (method != HTTP_POST) return false;
        admin_.handlePost(uri);
        return true;
    }

private:
    WebAdmin& admin_;
};

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
    // Every POST goes through one raw handler so the body is read only
    // after the credentials and the declared size have been checked.
    server_.addHandler(new PostHandler(*this));
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

void WebAdmin::receiveBody(HTTPRaw& raw)
{
    if (raw.status == RAW_START) {
        body_ = "";
        const int length = server_.clientContentLength();
        if (length > kMaxPostBody ||
            !server_.authenticate(username_.c_str(), token_.c_str())) {
            // Marking the body as already received makes the server skip
            // it; handlePost then answers 401 or 413.
            raw.totalSize = length > 0 ? length : 0;
        }
    } else if (raw.status == RAW_WRITE &&
               body_.length() + raw.currentSize <= kMaxPostBody) {
        body_.concat(reinterpret_cast<const char*>(raw.buf), raw.currentSize);
    }
}

void WebAdmin::handlePost(const String& uri)
{
    if (!authorize()) return;
    const char* refusal = nullptr;
    int status = 409;
    JsonDocument input;
    JsonDocument output;
    bool ok = false;
    if (server_.clientContentLength() > kMaxPostBody) {
        status = 413;
        refusal = "request too large";
    } else if (server_.header("X-Hermes-Admin") != "1") {
        status = 403;
        refusal = "admin header required";
    } else if (uri == "/api/interrupt") {
        ok = listener_->interruptWebSession();
    } else if (uri != "/api/auth/cookie" && uri != "/api/prompt") {
        status = 404;
        refusal = "not found";
    } else if (deserializeJson(input, body_)) {
        refusal = "invalid JSON";
    } else if (uri == "/api/prompt") {
        const String text = input["text"] | "";
        ok = text.length() && text.length() <= 4000 &&
             listener_->submitWebPrompt(text);
    } else {
        ok = listener_->updateWebAuthCookie(input["cookie"] | "");
        if (!ok) refusal = "cookie rejected or could not be saved";
    }
    body_ = "";
    output["ok"] = ok;
    if (refusal) output["error"] = refusal;
    sendJson(output, ok ? 202 : status);
}

void WebAdmin::sendJson(JsonDocument& document, int status)
{
    String body;
    serializeJson(document, body);
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(status, "application/json", body);
}

}  // namespace hermes_terminal
