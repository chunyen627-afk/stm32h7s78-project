/* Assetto Corsa Competizione 與 Assetto Corsa Rally：官方共享記憶體
 * （Local\acpmf_physics / graphics / static）。兩套用同一個版面：
 * Kunos《ACC Shared Memory Documentation v1.8.12》，#pragma pack(4)、小端序，
 * 三頁 800 / 1588 / 820 位元組。名稱一樣，所以靠行程名稱分辨是哪一套。
 *
 * AC Rally（搶先體驗）跟 ACC 的差別（社群在 0.2/0.3 版實測）：
 *  - 溫度是 Kelvin（大於 200 就減 273.15）
 *  - tcInAction / absInAction 沒有值、沒有單圈成績與油耗；iCurrentTime 是賽段計時
 *  - status 沒有驗證過，改看 physics packetId 有沒有在走
 * 只能唯讀開，不能自己建立對應。 */
#include "bridge.h"

#include <windows.h>
#include <string.h>

#define PH_SIZE 800
#define GR_SIZE 1588
#define ST_SIZE 820

enum { AC_OFF = 0, AC_REPLAY = 1, AC_LIVE = 2, AC_PAUSE = 3 };

static HANDLE         s_h[3];
static const uint8_t *s_v[3];
static int32_t        s_last_ph, s_last_gr;
static DWORD          s_ph_moved;       /* physics packetId 上次變動的時間（Rally 判斷在跑用） */

static int32_t i32(const uint8_t *p, int o) { int32_t v; memcpy(&v, p + o, 4); return v; }
static float   f32(const uint8_t *p, int o) { float v; memcpy(&v, p + o, 4); return v; }

static void acpmf_close(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_v[i]) { UnmapViewOfFile(s_v[i]); }
        if (s_h[i]) { CloseHandle(s_h[i]); }
        s_v[i] = NULL;
        s_h[i] = NULL;
    }
}

static bool acpmf_open(void)
{
    static const wchar_t *const names[3] = {
        L"Local\\acpmf_physics", L"Local\\acpmf_graphics", L"Local\\acpmf_static",
    };
    for (int i = 0; i < 3; i++) {
        s_h[i] = OpenFileMappingW(FILE_MAP_READ, FALSE, names[i]);
        if (!s_h[i]) { acpmf_close(); return false; }
        s_v[i] = (const uint8_t *)MapViewOfFile(s_h[i], FILE_MAP_READ, 0, 0, 0);
        if (!s_v[i]) { acpmf_close(); return false; }
    }
    s_last_ph = s_last_gr = -1;
    s_ph_moved = 0;
    logf_("acpmf 共享記憶體已開啟");
    return true;
}

static float temp_c(float v) { return v > 200.0f ? v - 273.15f : v; }   /* Rally 是 Kelvin */

static int acpmf_poll(tele_t *t, bool rally)
{
    static uint8_t ph[PH_SIZE], gr[GR_SIZE], st[ST_SIZE];

    for (int tries = 0; tries < 3; tries++) {
        int32_t a = i32(s_v[0], 0);
        memcpy(ph, s_v[0], PH_SIZE);
        memcpy(gr, s_v[1], GR_SIZE);
        if (i32(s_v[0], 0) == a) { break; }
    }
    memcpy(st, s_v[2], ST_SIZE);

    int32_t pid = i32(ph, 0), gid = i32(gr, 0);
    if (pid != s_last_ph) { s_ph_moved = GetTickCount(); }
    if (pid == s_last_ph && gid == s_last_gr) { return 0; }
    s_last_ph = pid;
    s_last_gr = gid;

    tele_clear(t);
    /* 暫停也算在跑（儀表停在暫停那一刻）；Rally 的 status 沒驗證過，看物理有沒有在走 */
    t->running   = rally ? (GetTickCount() - s_ph_moved < 300) : (i32(gr, 4) == AC_LIVE || i32(gr, 4) == AC_PAUSE);
    t->speed_kmh = f32(ph, 28);
    t->gear      = i32(ph, 16) - 1;                 /* 0=R、1=N、2=1 檔 */
    t->rpm       = (float)i32(ph, 20);
    t->max_rpm   = (float)i32(st, 412);             /* static maxRpm */
    if (t->max_rpm <= 0) { t->max_rpm = (float)i32(ph, 588); }   /* physics currentMaxRpm（int） */
    if (t->max_rpm <= 0) { t->max_rpm = NAN; }
    t->throttle  = f32(ph, 4);
    t->brake     = f32(ph, 8);
    for (int i = 0; i < 4; i++) { t->tyre_c[i] = temp_c(f32(ph, 152 + i * 4)); }   /* 胎芯 FL FR RL RR */
    t->fuel_l     = f32(ph, 12);
    t->fuel_max_l = f32(st, 416);
    if (t->fuel_max_l <= 0) { t->fuel_max_l = NAN; }
    t->cur_ms = i32(gr, 140);

    if (rally) {
        /* 賽段：沒有圈、沒有名次；TC/ABS 作動與油耗都沒發佈 */
        return 1;
    }
    t->tc  = i32(ph, 672) != 0;                     /* tcInAction */
    t->abs = i32(ph, 676) != 0;                     /* absInAction */
    /* 還沒有圈速時是很大的數字 */
    int32_t last = i32(gr, 144), best = i32(gr, 148);
    t->last_ms = (last > 0 && last < 3600000) ? last : 0;
    t->best_ms = (best > 0 && best < 3600000) ? best : 0;
    t->lap = i32(gr, 132) + 1;                      /* completedLaps + 1 */
    t->position = i32(gr, 136);
    t->invalid = i32(gr, 1408) == 0;                /* isValidLap */
    float est = f32(gr, 1412);                      /* fuelEstimatedLaps */
    t->fuel_laps = (est > 0 && est < 1000) ? est : NAN;
    /* ACC 是計時賽，numberOfLaps 其實是已完成圈數，沒有「總圈數」可用 */
    return 1;
}

static int acc_poll(tele_t *t)   { return acpmf_poll(t, false); }
static int rally_poll(tele_t *t) { return acpmf_poll(t, true); }

const source_t src_acc = {
    "Assetto Corsa Competizione", { "ac2-win64-shipping.exe", NULL }, acpmf_open, acc_poll, acpmf_close,
};
const source_t src_acrally = {
    "Assetto Corsa Rally", { "acr.exe", NULL }, acpmf_open, rally_poll, acpmf_close,
};
