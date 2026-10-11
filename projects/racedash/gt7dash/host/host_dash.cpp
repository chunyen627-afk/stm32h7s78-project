/* 電腦端參考渲染器：原版 SHCustomProtocol（不改一行）＋ LovyanGFX 記憶體畫布。
 * 用 DSH1 測試資料把每個主題畫出來存成圖，跟原作照片、跟板子畫面比對。 */
#define INCLUDE_GT7_WIFI true
#include <Arduino.h>
#include <WiFi.h>
#include "GT7UDPParser.h"

HardwareSerialStub Serial;
WiFiClass WiFi;
GT7_UDP_Parser gt7Telem;
Packet gt7Packet;

/* 原作在 ESP32 上自己讀序列埠用的函式；racedash 直接把整行交給它，用不到。 */
int  FlowSerialAvailable() { return 0; }
int  FlowSerialTimedRead() { return -1; }
void FlowSerialWrite(uint8_t) {}
void FlowSerialFlush() {}

/* racedash 平台掛勾（板子上由 race_main 提供） */
static std::map<std::string, unsigned char> g_prefs;
extern "C" int  racedash_pref_get(const char *k, unsigned char *v) { auto it = g_prefs.find(k); if (it == g_prefs.end()) return 0; *v = it->second; return 1; }
extern "C" void racedash_pref_put(const char *k, unsigned char v) { g_prefs[k] = v; }
extern "C" void racedash_pref_clear(void) { g_prefs.clear(); }
static int g_tx = -1, g_ty = -1;
extern "C" bool racedash_touch(int *x, int *y) { if (g_tx < 0) return false; *x = g_tx; *y = g_ty; return true; }
extern "C" void racedash_backlight(unsigned char) {}

#define private public      /* 只在這個驅動程式裡：直接把一行 DSH1 交給原作的解析 */
#include "SHCustomProtocol.h"
#undef private

#include <chrono>
static auto t0 = std::chrono::steady_clock::now();
static unsigned long g_fake_ms = 0;
unsigned long millis(void) { return g_fake_ms; }
unsigned long micros(void) { return g_fake_ms * 1000; }
void delay(unsigned long ms) { g_fake_ms += ms; }
namespace lgfx { inline namespace v1 {
  unsigned long millis(void) { return g_fake_ms; }
  unsigned long micros(void) { return g_fake_ms * 1000; }
  void delay(unsigned long ms) { g_fake_ms += ms; }
  void delayMicroseconds(unsigned int) {}
}}

static void dump(const char *path)
{
  FILE *f = fopen(path, "wb"); fprintf(f, "P6 320 240 255\n");
  for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
    uint16_t c = tft.readPixel(x, y);
    unsigned char rgb[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

/* 2 倍抗鋸齒畫布（板子實際顯示的那張），swap565 */
static bool dump_hi(const char *path)
{
  const uint16_t *b = lgfx::racedash::buffer(tft._racedash_hi);
  if (!b) return false;
  FILE *f = fopen(path, "wb"); fprintf(f, "P6 640 480 255\n");
  for (int i = 0; i < 640 * 480; i++) {
    uint16_t c = (uint16_t)((b[i] >> 8) | (b[i] << 8));
    unsigned char rgb[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  return true;
}

static void step(SHCustomProtocol &dash, const char *line, int i)
{
  g_fake_ms += 100;
  /* 序號每次要變，不然會被當成重複 */
  char seq[16]; snprintf(seq, sizeof(seq), "%d", 100 + i);
  String l = String(line); int a = l.indexOf(';'), b = l.indexOf(';', a + 1);
  String nl = l.substring(0, a + 1) + seq + l.substring(b);
  strncpy(dash.customLine, nl.c_str(), sizeof(dash.customLine) - 1);
  dash.acceptSimHubFrame();
  dash.loop();
}

/* 用法：host_dash [主題] [DSH1 一行] [跑幾格] [點擊 x,y ...]
 * 輸出 theme<主題>.ppm（原本的 320x240）與 theme<主題>_hi.ppm（2 倍抗鋸齒）。
 * 點擊座標是 320x240 的，每個點按住 3 格、放開 10 格，用來叫出設定畫面。 */
int main(int argc, char **argv)
{
  int theme = argc > 1 ? atoi(argv[1]) : 1;
  const char *line = argc > 2 ? argv[2] :
    "DSH1;42;1;87;2;6100;72;90;00:09.366;--;--;--;20;1;--;100;0;0;0;0;0;59.0;59.0;60.0;60.0";
  char out[64]; snprintf(out, sizeof(out), "theme%d.ppm", theme);

  racedash_pref_put("touchRot", 0);
  racedash_pref_put("connection", 2);           /* USB 序列埠模式 */
  racedash_pref_put("theme", (unsigned char)theme);
  static SHCustomProtocol dash;
  dash.setup();
  dash.initializeTelemetry();
  int iters = argc > 3 ? atoi(argv[3]) : 40;
  int n = 0;
  for (int i = 0; i < iters; i++) step(dash, line, n++);
  for (int k = 4; k < argc; k++) {
    if (sscanf(argv[k], "%d,%d", &g_tx, &g_ty) != 2) continue;
    for (int i = 0; i < 3; i++) step(dash, line, n++);
    g_tx = g_ty = -1;
    for (int i = 0; i < 10; i++) step(dash, line, n++);
  }
  dump(out);
  printf("wrote %s\n", out);
  snprintf(out, sizeof(out), "theme%d_hi.ppm", theme);
  if (dump_hi(out)) printf("wrote %s\n", out);
  return 0;
}
