#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

#include "hermes_terminal/config.h"

namespace hermes_terminal {

class HermesClientListener {
public:
    virtual ~HermesClientListener() = default;
    virtual void onHermesConnected() = 0;
    virtual void onHermesDisconnected(const String& reason) = 0;
    virtual void onHermesMessage(JsonDocument& message) = 0;
    virtual void onHermesAuthCookieUpdated(const String& cookie) = 0;
};

class HermesClient {
public:
    bool begin(const Config& config, const String& caPem,
               HermesClientListener& listener);
    void update();
    bool connected();
    bool connectionFailed() const;
    String diagnostic() const;
    String authMode() const;
    String gatewayAuthMode() const;
    String gatewayAuthFlows() const;
    bool gatewayAuthRequired() const { return gatewayAuthRequired_; }
    bool authConfigured() const;
    void setSessionCookie(const String& cookie);
    std::uint32_t request(const char* method, JsonObjectConst params);
    std::uint32_t request(const char* method);
    bool refreshAuthentication();
    void disconnect();

private:
    enum class State : std::uint8_t {
        kIdle,
        kWaitingWifi,
        kConnecting,
        kConnected,
        kBackoff,
    };

    bool connectNow();
    bool passwordLogin();
    bool obtainTicket(String& ticket);
    bool httpRequest(const char* method, const String& path,
                     const String& requestBody, const String& cookie,
                     int& status, String& headers, String& response);
    bool openWebSocket(const String& authName, const String& authValue);
    bool probeGatewayStatus();
    bool sendText(const String& text);
    bool sendFrame(std::uint8_t opcode, const std::uint8_t* data,
                   std::size_t length);
    bool readFrame();
    bool readExact(std::uint8_t* data, std::size_t length,
                   unsigned long timeoutMs);
    void handleText(const String& text);
    void fail(const String& reason);
    bool parseBaseUrl();

    Config config_;
    String caPem_;
    String host_;
    String basePath_;
    std::uint16_t port_ = 443;
    HermesClientListener* listener_ = nullptr;
    WiFiClientSecure socket_;
    State state_ = State::kIdle;
    String diagnostic_ = "NOT STARTED";
    String gatewayAuthMode_ = "unknown";
    String gatewayAuthFlows_;
    bool gatewayAuthRequired_ = false;
    bool statusProbeAttempted_ = false;
    unsigned long nextStatusProbeMs_ = 0;
    unsigned long nextPasswordLoginMs_ = 0;
    unsigned long nextConnectMs_ = 0;
    unsigned long lastReceiveMs_ = 0;
    unsigned long lastPingMs_ = 0;
    std::uint32_t nextRequestId_ = 1;
};

}  // namespace hermes_terminal
