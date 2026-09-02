#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
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
    using CancelCheck = bool (*)();

    struct RestDownloadResult {
        int status = 0;
        std::size_t bytes = 0;
        String path;
        String error;
        bool cancelled = false;
    };

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
    bool refreshAuthentication(CancelCheck cancelCheck = nullptr);
    bool beginRestDownload(const String& path, fs::FS& destination,
                           const String& outputPath);
    bool restDownloadActive() const;
    void cancelRestDownload();
    bool takeRestDownloadResult(RestDownloadResult& result);
    void disconnect();

private:
    enum class State : std::uint8_t {
        kIdle,
        kWaitingWifi,
        kConnecting,
        kConnected,
        kBackoff,
        kRestConnecting,
        kRestHeaders,
        kRestBody,
        kRestDone,
    };

    bool connectNow();
    bool passwordLogin(CancelCheck cancelCheck = nullptr);
    bool obtainTicket(String& ticket, CancelCheck cancelCheck = nullptr);
    bool httpRequest(const char* method, const String& path,
                     const String& requestBody, const String& cookie,
                     int& status, String& headers, String& response,
                     CancelCheck cancelCheck = nullptr);
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
    void updateRestDownload();
    void finishRestDownload(const String& error = "", bool cancelled = false);
    bool writeRestByte(std::uint8_t value);

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
    unsigned long reconnectDelayMs_ = 0;
    unsigned long lastReceiveMs_ = 0;
    unsigned long lastPingMs_ = 0;
    std::uint32_t nextRequestId_ = 1;
    fs::FS* restFs_ = nullptr;
    File restFile_;
    String restRequestPath_;
    String restOutputPath_;
    String restHeaders_;
    String restChunkLine_;
    int restStatus_ = 0;
    std::size_t restBytes_ = 0;
    std::size_t restContentLength_ = 0;
    std::size_t restChunkRemaining_ = 0;
    unsigned long restLastDataMs_ = 0;
    bool restChunked_ = false;
    bool restNeedChunkSize_ = false;
    std::uint8_t restChunkCrlf_ = 0;
    bool restResultReady_ = false;
    RestDownloadResult restResult_;
};

}  // namespace hermes_terminal
