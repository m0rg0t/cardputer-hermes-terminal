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
struct WiFiStub {
    String SSID() const;
    IPAddress localIP() const;
    int RSSI() const;
};
extern WiFiStub WiFi;
