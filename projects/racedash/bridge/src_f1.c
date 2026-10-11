/* EA SPORTS F1 25（也收 F1 24）：官方 UDP 遙測（遊戲設定 → Telemetry Settings → UDP Telemetry 開、
 * Port 20777 或 20888、Send Rate 60Hz）。依 EA《Data Output from F1 25 v3》與 2026 Season Pack 規格。
 *
 * 封包開頭 29 位元組的 header：m_packetFormat（2025 或 2026）、m_packetId（偏移 6）、
 * m_sessionUID（7，uint64）、m_playerCarIndex（27）。玩家車的資料在
 * 29 + 玩家索引 × 每台車大小 + 欄位偏移。兩種格式我們用到的欄位偏移都一樣，
 * 只有每台車的大小不同：Car Telemetry 60 / 59、Car Status 55 / 59、Lap Data 都是 57。
 *
 * 沒有的資料：TC / ABS 只有設定值，沒有「正在介入」→ 顯示 "--"；
 * 最佳圈沒有直接欄位 → 自己記（換圈時那圈有效就比較）。 */
#include "bridge.h"

#include <windows.h>
#include <string.h>

enum { PKT_SESSION = 1, PKT_LAP = 2, PKT_TELEMETRY = 6, PKT_STATUS = 7 };

static struct {
    bool     have_tel;
    DWORD    last_rx;           /* 上次收到 Car Telemetry 的時間 */
    bool     updated;           /* poll 之後有沒有新資料 */
    uint64_t session_uid;
    /* 玩家車 */
    float    speed, throttle, brake, rpm, max_rpm;
    int      gear;
    float    tyre_c[4];         /* FL FR RL RR */
    float    fuel, fuel_cap;
    int32_t  cur_ms, last_ms, best_ms;
    int      lap, position, invalid, total_laps;
    int      prev_lap, prev_invalid;
} s;

static uint16_t u16(const uint8_t *p, int o) { uint16_t v; memcpy(&v, p + o, 2); return v; }
static uint32_t u32(const uint8_t *p, int o) { uint32_t v; memcpy(&v, p + o, 4); return v; }
static float    f32(const uint8_t *p, int o) { float v; memcpy(&v, p + o, 4); return v; }

void f1_feed(const uint8_t *p, int n)
{
    int      fmt = u16(p, 0);
    int      id = p[6];
    int      car = p[27];
    uint64_t uid;

    /* 2024 跟 2025 這幾種封包的版面完全一樣（EA 規格比對過） */
    if (fmt < 2024 || fmt > 2026 || car >= (fmt == 2026 ? 24 : 22)) { return; }
    memcpy(&uid, p + 7, 8);
    if (uid != s.session_uid) {                      /* 新的一場：清掉最佳圈與換圈紀錄 */
        s.session_uid = uid;
        s.best_ms = 0;
        s.prev_lap = 0;
        s.total_laps = 0;
    }

    if (id == PKT_TELEMETRY) {
        int o = 29 + car * (fmt == 2026 ? 59 : 60);
        if (o + 38 > n) { return; }
        s.speed    = (float)u16(p, o + 0);
        s.throttle = f32(p, o + 2);
        s.brake    = f32(p, o + 10);
        s.gear     = (int8_t)p[o + 15];             /* -1 = R、0 = N */
        s.rpm      = (float)u16(p, o + 16);
        /* 胎芯溫度 uint8，F1 的順序是 RL RR FL FR */
        s.tyre_c[0] = p[o + 36];
        s.tyre_c[1] = p[o + 37];
        s.tyre_c[2] = p[o + 34];
        s.tyre_c[3] = p[o + 35];
        s.have_tel = true;
        s.last_rx = GetTickCount();
        s.updated = true;
    } else if (id == PKT_STATUS) {
        int o = 29 + car * (fmt == 2026 ? 59 : 55);
        if (o + 21 > n) { return; }
        s.fuel     = f32(p, o + 5);                 /* kg（油耗也用 kg 算，單位一致） */
        s.fuel_cap = f32(p, o + 9);
        s.max_rpm  = (float)u16(p, o + 17);
    } else if (id == PKT_LAP) {
        int o = 29 + car * 57;
        if (o + 38 > n) { return; }
        s.last_ms  = (int32_t)u32(p, o + 0);
        s.cur_ms   = (int32_t)u32(p, o + 4);
        s.position = p[o + 32];
        s.lap      = p[o + 33];
        s.invalid  = p[o + 37] != 0;
        /* 換圈：剛跑完的那圈有效就拿來比最佳圈 */
        if (s.prev_lap && s.lap == s.prev_lap + 1 && !s.prev_invalid && s.last_ms > 0 &&
            (s.best_ms == 0 || s.last_ms < s.best_ms)) {
            s.best_ms = s.last_ms;
        }
        s.prev_lap = s.lap;
        s.prev_invalid = s.invalid;
    } else if (id == PKT_SESSION) {
        if (n > 32) { s.total_laps = p[32]; }
    }
}

static bool f1_open(void) { return true; }          /* UDP 埠由 main 一開始就開了 */
static void f1_close(void) {}

static int f1_poll(tele_t *t)
{
    udp_pump();
    if (s.have_tel && GetTickCount() - s.last_rx > 2000) { s.have_tel = false; }
    if (!s.updated) { return 0; }
    s.updated = false;

    tele_clear(t);
    t->running   = true;                            /* 有 Car Telemetry 就是在場上 */
    t->speed_kmh = s.speed;
    t->gear      = s.gear;
    t->rpm       = s.rpm;
    t->max_rpm   = s.max_rpm > 0 ? s.max_rpm : NAN;
    t->throttle  = s.throttle;
    t->brake     = s.brake;
    for (int i = 0; i < 4; i++) { t->tyre_c[i] = s.tyre_c[i]; }
    if (s.fuel_cap > 0) { t->fuel_l = s.fuel; t->fuel_max_l = s.fuel_cap; }
    t->cur_ms   = s.cur_ms;
    t->last_ms  = s.last_ms;
    t->best_ms  = s.best_ms;
    if (s.lap > 0) { t->lap = s.lap; }
    if (s.position > 0) { t->position = s.position; }
    t->total_laps = s.total_laps;
    t->invalid  = s.invalid;
    return 1;
}

const source_t src_f1 = { "F1", { "f1_25.exe", "f1_24.exe", NULL }, f1_open, f1_poll, f1_close };
