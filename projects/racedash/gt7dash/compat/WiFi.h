#pragma once
#include "Arduino.h"
#include "esp_wifi.h"
typedef enum { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL, WL_SCAN_COMPLETED, WL_CONNECTED, WL_CONNECT_FAILED, WL_CONNECTION_LOST, WL_DISCONNECTED } wl_status_t;
typedef enum { WIFI_OFF = 0, WIFI_STA, WIFI_AP, WIFI_AP_STA } wifi_mode_t;
class IPAddress
{
public:
    IPAddress() {}
    IPAddress(uint8_t, uint8_t, uint8_t, uint8_t) {}
    String toString() const { return "0.0.0.0"; }
    operator uint32_t() const { return 0; }
};
class WiFiClass
{
public:
    bool mode(wifi_mode_t) { return true; }
    wl_status_t status() { return WL_DISCONNECTED; }
    IPAddress localIP() { return IPAddress(); }
    String SSID() { return ""; }
};
extern WiFiClass WiFi;
