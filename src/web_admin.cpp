#include "hermes_terminal/web_admin.h"

#include <WiFi.h>

namespace hermes_terminal {
namespace {

const char kAdminPage[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8><meta name=viewport content="width=device-width">
<title>Hermes Terminal</title><style>
body{font:16px system-ui;max-width:720px;margin:32px auto;padding:0 16px;background:#101318;color:#e8edf2}
button,input,textarea{font:inherit}input{box-sizing:border-box;width:100%;padding:10px;background:#181d24;color:#fff;border:1px solid #52606d;border-radius:6px}
textarea{box-sizing:border-box;width:100%;height:130px;padding:10px;background:#181d24;color:#fff;border:1px solid #52606d;border-radius:6px}
button{margin:8px 8px 8px 0;padding:8px 14px}.muted{color:#9aa6b2}pre{white-space:pre-wrap}.card{border:1px solid #303944;border-radius:8px;padding:12px;margin:16px 0}
</style></head><body><h1>Hermes Terminal</h1><pre id=s>Loading...</pre>
<section class=card><h2>Remote Hermes sign-in</h2>
<p class=muted><b>Trusted LAN only:</b> this admin page is plain HTTP. Pasting a Cookie sends a high-privilege Hermes session over the LAN. Use it only on a trusted network, then disable web_admin. Paste the complete Cookie header, including access and refresh cookies, for example <code>hermes_session_at=...; hermes_session_rt=...</code>. The value is write-only here and is never returned in status.</p>
<input id=c type=password autocomplete=off placeholder="hermes_session_at=...; hermes_session_rt=..."><br>
<button onclick=saveCookie()>Save Cookie and reconnect</button><span id=m></span></section>
<textarea id=p placeholder="Prompt active Hermes session"></textarea><br>
<button onclick=send()>Send</button><button onclick=stop()>Interrupt</button>
<p class=muted>Opt-in LAN control. Audio and stored credentials are not included in status responses.</p><script>
async function api(path,opt){let r=await fetch(path,opt);let t=await r.text();if(!r.ok)throw Error(t);return t?JSON.parse(t):{}}
async function status(){try{s.textContent=JSON.stringify(await api('/api/status'),null,2)}catch(e){s.textContent=e}setTimeout(status,2000)}
async function saveCookie(){let value=c.value.trim();if(!value)return;m.textContent=' Saving...';try{await api('/api/auth/cookie',{method:'POST',headers:{'Content-Type':'application/json','X-Hermes-Admin':'1'},body:JSON.stringify({cookie:value})});c.value='';m.textContent=' Saved; reconnecting.'}catch(e){m.textContent=' '+e} }
async function send(){let text=p.value.trim();if(!text)return;await api('/api/prompt',{method:'POST',headers:{'Content-Type':'application/json','X-Hermes-Admin':'1'},body:JSON.stringify({text})});p.value=''}
async function stop(){await api('/api/interrupt',{method:'POST',headers:{'X-Hermes-Admin':'1'}})}status();
</script></body></html>)HTML";

}  // namespace

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
        server_.send_P(200, "text/html; charset=utf-8", kAdminPage);
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
        const auto error = deserializeJson(input, server_.arg("plain"));
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
