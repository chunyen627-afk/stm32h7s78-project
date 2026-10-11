/* Assetto Corsa EVO：官方共享記憶體（Local\acevo_pmf_physics / graphics / static）。
 *
 * 版面依 Kunos 官方文件 ACE_SharedFileOut_Documentation_v1（2026-04-01），#pragma pack(4)、
 * 小端序；三頁大小 800 / 4900 / 208，跟 AssettoCorsaEVO.exe（build 24989348）裡寫死的一樣。
 * 只能用 OpenFileMapping 唯讀開：自己建立對應的話，遊戲啟動時發現大小不對會整場不送遙測。
 * 沒有同步機制：先複製整頁，前後的 packetId 一樣才用。 */
#include "bridge.h"

#include <windows.h>
#include <string.h>

#define PH_SIZE 800
#define GR_SIZE 4900
#define ST_SIZE 208

enum { AC_OFF = 0, AC_REPLAY = 1, AC_LIVE = 2, AC_PAUSE = 3 };

static HANDLE         s_h[3];
static const uint8_t *s_v[3];          /* physics, graphics, static */
static int32_t        s_last_ph, s_last_gr;

static int32_t  i32(const uint8_t *p, int o) { int32_t v; memcpy(&v, p + o, 4); return v; }
static uint32_t u32(const uint8_t *p, int o) { uint32_t v; memcpy(&v, p + o, 4); return v; }
static float    f32(const uint8_t *p, int o) { float v; memcpy(&v, p + o, 4); return v; }

static void acevo_close(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_v[i]) { UnmapViewOfFile(s_v[i]); }
        if (s_h[i]) { CloseHandle(s_h[i]); }
        s_v[i] = NULL;
        s_h[i] = NULL;
    }
}

static bool acevo_open(void)
{
    static const wchar_t *const names[3] = {
        L"Local\\acevo_pmf_physics", L"Local\\acevo_pmf_graphics", L"Local\\acevo_pmf_static",
    };
    for (int i = 0; i < 3; i++) {
        s_h[i] = OpenFileMappingW(FILE_MAP_READ, FALSE, names[i]);
        if (!s_h[i]) { acevo_close(); return false; }       /* 遊戲還沒建好，等一下再試 */
        s_v[i] = (const uint8_t *)MapViewOfFile(s_h[i], FILE_MAP_READ, 0, 0, 0);
        if (!s_v[i]) { acevo_close(); return false; }
    }
    s_last_ph = s_last_gr = -1;
    logf_("AC EVO 共享記憶體已開啟（版本 %.15s / 遊戲 %.15s）",
          (const char *)s_v[2], (const char *)s_v[2] + 15);
    return true;
}

static int acevo_poll(tele_t *t)
{
    static uint8_t ph[PH_SIZE], gr[GR_SIZE], st[ST_SIZE];

    /* 複製一份；複製途中遊戲寫了新的就重來 */
    for (int tries = 0; tries < 3; tries++) {
        int32_t a = i32(s_v[0], 0);
        memcpy(ph, s_v[0], PH_SIZE);
        memcpy(gr, s_v[1], GR_SIZE);
        if (i32(s_v[0], 0) == a) { break; }
    }
    memcpy(st, s_v[2], ST_SIZE);

    int32_t pid = i32(ph, 0), gid = i32(gr, 0);
    if (pid == s_last_ph && gid == s_last_gr) { return 0; }
    s_last_ph = pid;
    s_last_gr = gid;

    /* 暫停時遊戲把物理資料清成 0（檔位會變成 R、時速 0），所以停在暫停前最後一刻的
     * 數值；暫停也算在跑，不要切到等待畫面。回到選單（OFF）才算沒在跑。 */
    static tele_t s_live;
    static bool   s_have_live;
    if (i32(gr, 4) == AC_PAUSE && s_have_live) {
        *t = s_live;
        return 1;
    }

    tele_clear(t);
    t->running   = i32(gr, 4) == AC_LIVE || i32(gr, 4) == AC_PAUSE;
    t->speed_kmh = f32(ph, 28);
    t->gear      = i32(ph, 16) - 1;                 /* 0=R、1=N、2=1 檔 → -1、0、1 */
    t->rpm       = (float)i32(ph, 20);
    t->max_rpm   = (float)i32(ph, 588);             /* currentMaxRpm（static 頁沒有最大轉速） */
    if (t->max_rpm <= 0) { t->max_rpm = NAN; }
    t->throttle  = f32(ph, 4);
    t->brake     = f32(ph, 8);
    t->tc        = i32(ph, 672) != 0;               /* tcinAction */
    t->abs       = i32(ph, 676) != 0;               /* absInAction */
    for (int i = 0; i < 4; i++) { t->tyre_c[i] = f32(ph, 152 + i * 4); }   /* 胎芯溫度 FL FR RL RR */

    t->fuel_l     = f32(gr, 196);
    t->fuel_max_l = f32(gr, 3928);
    if (t->fuel_max_l <= 0) { t->fuel_max_l = NAN; }
    t->cur_ms  = i32(gr, 188);
    t->last_ms = i32(gr, 2396) > 0 ? i32(gr, 2396) : 0;
    t->best_ms = i32(gr, 2400) > 0 ? i32(gr, 2400) : 0;
    t->lap = i32(gr, 2548);                         /* session_state.current_lap */
    if (t->lap <= 0) { t->lap = i32(gr, 2384) + 1; }   /* 已完成圈數 + 1 */
    t->total_laps = st[85] ? 0 : i32(gr, 2544);     /* 計時賽沒有總圈數 */
    t->position = (int)u32(gr, 2388);
    t->invalid = !gr[3121] || gr[2849];             /* is_valid_lap、timing_state.is_invalid */
    if (i32(gr, 4) == AC_LIVE) { s_live = *t; s_have_live = true; }
    return 1;
}

const source_t src_acevo = {
    "Assetto Corsa EVO", { "assettocorsaevo.exe", NULL }, acevo_open, acevo_poll, acevo_close,
};
