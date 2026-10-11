/* Forza Motorsport（2023）與 Forza Horizon 6：官方 Data Out（UDP）。
 * 依官方文件「Forza Motorsport Data Out Documentation」與「Forza Horizon 6 Data Out Documentation」。
 *
 * 遊戲設定：
 *   FM：SETTINGS > GAMEPLAY & HUD > UDP RACE TELEMETRY：Data Out 開、IP 127.0.0.1、
 *       Port 20777、Packet Format 選 Dash（選 Sled 沒有速度與檔位）
 *   FH6：SETTINGS > HUD AND GAMEPLAY：Data Out 開、IP 127.0.0.1、Port 20777
 *       （不要用 5200~5300，遊戲自己佔用）
 *
 * 依封包長度分：331 = FM2023 Dash、311 = FM7 Dash（儀表區從 232 開始）；
 * 324 / 323 = Forza Horizon（中間多 12 位元組，儀表區從 244 開始）；232 = Sled，不夠用。
 * Horizon 只有開車時才送（選單、暫停都停），所以靠「多久沒收到」判斷有沒有在跑。
 * 沒有的資料：TC / ABS（顯示 "--"）、油箱公升數（只有 0..1 的比例）、總圈數。 */
#include "bridge.h"

#include <windows.h>
#include <string.h>

static struct {
    bool  updated;
    DWORD last_rx;
    bool  race_on;
    float rpm, max_rpm, speed_kmh, throttle, brake, fuel_pct;
    float tyre_c[4];
    int   gear;
    int32_t cur_ms, last_ms, best_ms;
    int   lap, position;
} s;

static float    f32(const uint8_t *p, int o) { float v; memcpy(&v, p + o, 4); return v; }
static int32_t  s32(const uint8_t *p, int o) { int32_t v; memcpy(&v, p + o, 4); return v; }
static uint16_t u16(const uint8_t *p, int o) { uint16_t v; memcpy(&v, p + o, 2); return v; }

static int32_t sec_to_ms(float sec) { return (sec > 0.0f && sec < 36000.0f) ? (int32_t)(sec * 1000.0f + 0.5f) : 0; }

void forza_feed(const uint8_t *p, int n)
{
    int b;                                  /* 儀表區（Dash）起點 */

    if (n == 331 || n == 311) { b = 232; }
    else if (n == 324 || n == 323) { b = 244; }
    else { return; }                        /* Sled 格式沒有速度、檔位、圈速 */

    s.race_on   = s32(p, 0) != 0;
    s.max_rpm   = f32(p, 8);
    s.rpm       = f32(p, 16);
    s.speed_kmh = f32(p, b + 12) * 3.6f;    /* m/s */
    for (int i = 0; i < 4; i++) {           /* 華氏，FL FR RL RR */
        s.tyre_c[i] = (f32(p, b + 24 + i * 4) - 32.0f) * 5.0f / 9.0f;
    }
    s.fuel_pct  = f32(p, b + 44) * 100.0f;
    s.best_ms   = sec_to_ms(f32(p, b + 52));
    s.last_ms   = sec_to_ms(f32(p, b + 56));
    s.cur_ms    = sec_to_ms(f32(p, b + 60));
    s.lap       = u16(p, b + 68) + 1;       /* LapNumber 是已完成圈數 */
    s.position  = p[b + 70];
    s.throttle  = p[b + 71] / 255.0f;
    s.brake     = p[b + 72] / 255.0f;
    /* 檔位：0 = R、1~10 前進檔、11 = N（FM 換檔途中也會報 11，那時保持原本的檔位） */
    int g = p[b + 75];
    if (g == 0) { s.gear = -1; }
    else if (g <= 10) { s.gear = g; }
    else if (s.speed_kmh < 5.0f || s.gear <= 0) { s.gear = 0; }
    s.last_rx = GetTickCount();
    s.updated = true;
}

static bool forza_open(void) { return true; }
static void forza_close(void) {}

static int forza_poll(tele_t *t)
{
    udp_pump();
    if (!s.updated) { return 0; }
    s.updated = false;

    tele_clear(t);
    t->running   = s.race_on;
    t->speed_kmh = s.speed_kmh;
    t->gear      = s.gear;
    t->rpm       = s.rpm;
    t->max_rpm   = s.max_rpm > 0 ? s.max_rpm : NAN;
    t->throttle  = s.throttle;
    t->brake     = s.brake;
    for (int i = 0; i < 4; i++) { t->tyre_c[i] = s.tyre_c[i]; }
    t->fuel_pct  = s.fuel_pct;
    t->fuel_l    = s.fuel_pct;              /* 只有比例：油耗估算用百分比，單位一致就好 */
    t->cur_ms    = s.cur_ms;
    t->last_ms   = s.last_ms;
    t->best_ms   = s.best_ms;
    if (s.position > 0) { t->position = s.position; t->lap = s.lap; }   /* 自由駕駛沒有圈與名次 */
    return 1;
}

const source_t src_forza = {
    "Forza",
    { "forza_steamworks_release_final.exe", "forza_gaming.desktop.x64_release_final.exe", "forzahorizon6.exe", NULL },
    forza_open, forza_poll, forza_close,
};
