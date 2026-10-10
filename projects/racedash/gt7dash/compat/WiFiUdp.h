#pragma once
#include "WiFi.h"
class WiFiUDP
{
public:
    uint8_t begin(uint16_t) { return 1; }
    void stop() {}
    int beginPacket(IPAddress, uint16_t) { return 1; }
    int endPacket() { return 1; }
    size_t write(const uint8_t *, size_t n) { return n; }
    size_t write(uint8_t) { return 1; }
    int parsePacket() { return 0; }
    int read(unsigned char *, size_t) { return 0; }
    int read() { return -1; }
    int available() { return 0; }
    IPAddress remoteIP() { return IPAddress(); }
    void flush() {}
};
