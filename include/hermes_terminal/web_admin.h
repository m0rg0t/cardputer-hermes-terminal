#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include "hermes_terminal/config.h"

namespace hermes_terminal {

class WebAdminListener {
public:
    virtual ~WebAdminListener() = default;
    virtual void writeWebStatus(JsonObject output) = 0;
    virtual bool updateWebAuthCookie(const String& cookie) = 0;
    virtual bool submitWebPrompt(const String& text) = 0;
    virtual bool interruptWebSession() = 0;
};

class WebAdmin {
public:
    bool begin(const Config& config, WebAdminListener& listener);
    void update();
    bool enabled() const { return enabled_; }

private:
    bool authorize();
    void sendJson(JsonDocument& document, int status = 200);

    WebServer server_{80};
    WebAdminListener* listener_ = nullptr;
    String username_;
    String token_;
    bool enabled_ = false;
};

}  // namespace hermes_terminal
