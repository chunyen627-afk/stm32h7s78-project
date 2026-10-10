/* 板子沒有 Wi-Fi：永遠「沒有存過的網路」。 */
#pragma once
#include <stdint.h>
#define ESP_OK 0
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP } wifi_interface_t;
typedef struct { struct { uint8_t ssid[32]; uint8_t password[64]; } sta; } wifi_config_t;
static inline int esp_wifi_get_config(wifi_interface_t, wifi_config_t *c) { c->sta.ssid[0] = 0; return ESP_OK; }
