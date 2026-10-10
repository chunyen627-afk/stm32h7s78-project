/* racedash 與原作儀表（caa1211/esp32-gt7-dashboard，MIT）之間的銜接層。
 *
 * 原作的 SHCustomProtocol（整個儀表 App：資料處理、七個主題、觸控選單）
 * 一行不改地編進來，畫在一張 320x240 的 LovyanGFX 記憶體畫布上；
 * race_main.c 再把畫布放大貼到 7 吋面板。對 C 這邊只開四個函式（gt7dash.h）。
 *
 * 原作在 ESP32 上的 Arduino、Wi-Fi、NVS 等由 compat/ 提供空殼或對應實作。 */
#define INCLUDE_GT7_WIFI true
#include <Arduino.h>
#include <WiFi.h>
#include "GT7UDPParser.h"

extern "C" {
#include "gt7dash.h"
uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t ms);
}

HardwareSerialStub Serial;
WiFiClass WiFi;
GT7_UDP_Parser gt7Telem;
Packet gt7Packet;

/* 原作走 SimHub Arduino 協定時用的序列埠函式。racedash 直接把整行交給它，用不到。 */
int  FlowSerialAvailable() { return 0; }
int  FlowSerialTimedRead() { return -1; }
void FlowSerialWrite(uint8_t) {}
void FlowSerialFlush() {}

/* 原作把收到的一行放進私有的 customLine 再呼叫私有的 acceptSimHubFrame()。
 * 為了不改原作，只在這個檔案把 private 打開，直接走它自己的解析流程。 */
#define private public
#include "SHCustomProtocol.h"
#undef private

/* ---- 時間（原作與 LovyanGFX 都要） ---- */
unsigned long millis(void) { return HAL_GetTick(); }
unsigned long micros(void) { return HAL_GetTick() * 1000UL; }
void delay(unsigned long ms) { HAL_Delay(ms); }
namespace lgfx { inline namespace v1 {
  unsigned long millis(void) { return HAL_GetTick(); }
  unsigned long micros(void) { return HAL_GetTick() * 1000UL; }
  void delay(unsigned long ms) { HAL_Delay(ms); }
  void delayMicroseconds(unsigned int) {}
}}

static SHCustomProtocol *s_dash;
static uint32_t s_seq;

extern "C" void gt7dash_init(void)
{
    static SHCustomProtocol dash;      /* 建構時會用到 String 等，等 C 這邊準備好才建 */

    s_dash = &dash;
    /* 沒有存過設定就是第一次開機：原作會進「觸控校正 -> 選連線方式」。
     * 板子只有 SimHub 這條路，而且觸控方向固定，直接設好跳過。 */
    unsigned char v;
    if (!racedash_pref_get("touchRot", &v))   { racedash_pref_put("touchRot", 0); }
    if (!racedash_pref_get("connection", &v)) { racedash_pref_put("connection", 2); }  /* TelemetryMode::SimHub */
    dash.setup();
    dash.initializeTelemetry();
}

extern "C" int gt7dash_feed(const char *line)
{
    if (!s_dash) { return 0; }
    strncpy(s_dash->customLine, line, sizeof(s_dash->customLine) - 1);
    s_dash->customLine[sizeof(s_dash->customLine) - 1] = 0;
    s_seq++;
    return s_dash->acceptSimHubFrame() ? 1 : 0;
}

extern "C" void gt7dash_step(void)
{
    if (s_dash) { s_dash->loop(); }
}

extern "C" const uint16_t *gt7dash_canvas(void)
{
    return (const uint16_t *)tft.getBuffer();
}

extern "C" const uint16_t *gt7dash_canvas_hi(void)
{
    return lgfx::racedash::buffer(tft._racedash_hi);
}

extern "C" void gt7dash_hires_set(int on)
{
    lgfx::racedash::set_enabled(on != 0);
}

extern "C" int gt7dash_canvas_hi_dirty(int *y0, int *y1)
{
    int32_t a, b;
    if (!lgfx::racedash::take_dirty(tft._racedash_hi, &a, &b)) { return 0; }
    *y0 = a;
    *y1 = b;
    return 1;
}

/* 除錯：直接切主題（0 Classic、1 GT3、2 Retro、3 Radar、4 Mono、5 Pocket、6 Endurance），
 * 跟使用者在原作選單裡選一樣，也會記進設定。截圖驗證時用，不用人去點。 */
extern "C" void gt7dash_set_theme(int theme)
{
    if (!s_dash || theme < 0 || theme > 6) { return; }
    s_dash->activeDashboardTheme = static_cast<DashboardTheme>(theme);
    racedash_pref_put("theme", (unsigned char)theme);
}
