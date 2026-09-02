#pragma once
#include <Arduino.h>

class IPAddress {
public:
    String toString() const { return text_; }
    IPAddress() = default;
    explicit IPAddress(const char* text) : text_(text) {}
private:
    String text_;
};

// Network facts the Status page prints; the harness fills them per scenario.
typedef enum { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 } wl_status_t;

struct WiFiStub {
    wl_status_t status() const;
    String SSID() const;
    IPAddress localIP() const;
    int RSSI() const;
};
extern WiFiStub WiFi;
