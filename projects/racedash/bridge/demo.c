/* 展示模式（系統匣右鍵「展示模式」或 --demo）：不用開遊戲，送模擬跑圈的資料給
 * 儀表（H7 與 2.8 吋都會動）。模擬的內容跟 H7 板子上 DEMO 按鈕的一樣
 * （照抄 app_src/race_main.c 的 demo_line，改那邊記得一起改）。 */
#include "bridge.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t ms;        /* 這段的長度 */
    uint8_t  mode;      /* 0 全油門  1 煞車到 target  2 彎中維持 target */
    uint8_t  target;    /* 彎速 km/h */
} demo_seg_t;

static const demo_seg_t k_demo_track[] = {
    {6500, 0,   0}, {1800, 1,  92}, {2600, 2,  92},
    {8200, 0,   0}, {1400, 1, 145}, {1800, 2, 145},
    {5200, 0,   0}, {2200, 1,  68}, {3000, 2,  68},
    {9800, 0,   0}, {1600, 1, 118}, {2400, 2, 118},
    {7000, 0,   0}, {1200, 1, 172}, {1500, 2, 172},
    {4800, 0,   0}, {2000, 1,  84}, {2600, 2,  84},
    {6000, 0,   0},
};
#define DEMO_SEGS    (int)(sizeof(k_demo_track) / sizeof(k_demo_track[0]))
#define DEMO_LAPS    10
#define DEMO_MAX_RPM 8500.0f

static struct {
    bool     started;
    uint32_t last_now, lap_start, rng;
    int      seg, gear, lap, pos;
    uint32_t seg_t, seg_len, last_ms, best_ms;
    float    target, speed, fuel, tyre[4];
    bool     invalid, invalid_next;
} g_sim;

static uint32_t sim_rand(void)
{
    g_sim.rng = g_sim.rng * 1664525u + 1013904223u;
    return g_sim.rng >> 8;
}

/* 進到第 seg 段：長度 ±5%、彎速 ±6% 的變化 */
static void sim_enter_segment(int seg)
{
    const demo_seg_t *d = &k_demo_track[seg];

    g_sim.seg = seg;
    g_sim.seg_t = 0;
    g_sim.seg_len = d->ms * (950u + sim_rand() % 101u) / 1000u;
    g_sim.target = d->target * (0.94f + (float)(sim_rand() % 121u) / 1000.0f);
    if (d->mode == 2 && g_sim.invalid_next && (sim_rand() % 3u) == 0u) {
        g_sim.invalid = true;           /* 這個彎出界，這圈不算 */
        g_sim.invalid_next = false;
    }
}

static void sim_reset(uint32_t now)
{
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.started = true;
    g_sim.rng = now ^ 0x5EED1234u;
    g_sim.last_now = now;
    g_sim.lap_start = now;
    g_sim.gear = 1;
    g_sim.lap = 1;
    g_sim.pos = 5;
    g_sim.fuel = 96.0f;
    for (int i = 0; i < 4; i++) { g_sim.tyre[i] = 74.0f + (float)i; }
    sim_enter_segment(0);
}

static void sim_fmt_lap(char *out, size_t cap, uint32_t ms)
{
    if (ms == 0u) { snprintf(out, cap, "--"); return; }
    snprintf(out, cap, "%02lu:%02lu.%03lu", (unsigned long)(ms / 60000u),
             (unsigned long)((ms / 1000u) % 60u), (unsigned long)(ms % 1000u));
}

int demo_line(char *out, int cap, uint32_t now)
{
    static const float k_kph_per_krpm[6] = {7.6f, 11.8f, 16.0f, 20.4f, 24.8f, 29.0f};
    static const float k_accel[6] = {30.0f, 24.0f, 18.0f, 13.0f, 9.0f, 6.5f};   /* km/h 每秒 */
    static uint32_t seq;
    const demo_seg_t *d;
    uint32_t dt;
    float    dts, thr = 0.0f, brk = 0.0f, rpm;
    bool     abs_on = false, tc_on = false;
    char     cur[16], last[16], best[16];

    if (!g_sim.started) { sim_reset(now); }
    dt = now - g_sim.last_now;
    if (dt > 100u) { dt = 100u; }
    g_sim.last_now = now;
    dts = (float)dt / 1000.0f;

    /* 這一段跑完就換下一段；最後一段跑完就是一圈 */
    g_sim.seg_t += dt;
    if (g_sim.seg_t >= g_sim.seg_len) {
        int next = g_sim.seg + 1;
        if (next >= DEMO_SEGS) {
            uint32_t lap_ms = now - g_sim.lap_start;
            g_sim.last_ms = lap_ms;
            if (!g_sim.invalid && (g_sim.best_ms == 0u || lap_ms < g_sim.best_ms)) { g_sim.best_ms = lap_ms; }
            g_sim.lap_start = now;
            g_sim.lap = g_sim.lap >= DEMO_LAPS ? 1 : g_sim.lap + 1;
            if (sim_rand() % 3u == 0u) {                        /* 超車或被超 */
                g_sim.pos += (sim_rand() & 1u) ? -1 : 1;
                if (g_sim.pos < 1) { g_sim.pos = 1; }
                if (g_sim.pos > 12) { g_sim.pos = 12; }
            }
            g_sim.invalid = false;
            g_sim.invalid_next = (sim_rand() % 6u) == 0u;
            if (g_sim.fuel < 8.0f) { g_sim.fuel = 100.0f; }   /* 進站加油 */
            next = 0;
        }
        sim_enter_segment(next);
    }
    d = &k_demo_track[g_sim.seg];

    /* 油門、煞車與速度 */
    switch (d->mode) {
    case 0:
        thr = 100.0f;
        g_sim.speed += (k_accel[g_sim.gear - 1] - 0.00012f * g_sim.speed * g_sim.speed) * dts;
        tc_on = g_sim.gear <= 3 && g_sim.seg_t < 1500u && ((now / 70u) & 1u);   /* 出彎打滑 */
        break;
    case 1:
        if (g_sim.speed > g_sim.target + 1.0f) {
            brk = 88.0f;
            g_sim.speed -= (55.0f + 0.00012f * g_sim.speed * g_sim.speed) * dts;
            abs_on = g_sim.speed > 60.0f && g_sim.seg_t < g_sim.seg_len * 6u / 10u && (now / 90u) % 3u != 0u;
        } else {
            thr = 20.0f;
        }
        break;
    default:
        thr = 42.0f + (float)(sim_rand() % 16u);
        g_sim.speed += (g_sim.target - g_sim.speed) * 0.8f * dts;
        break;
    }
    if (g_sim.speed < 0.0f) { g_sim.speed = 0.0f; }

    /* 檔位與轉速：到頂升檔、轉速掉了降檔（煞車時早一點降） */
    rpm = g_sim.speed / k_kph_per_krpm[g_sim.gear - 1] * 1000.0f;
    if (rpm > 8100.0f && g_sim.gear < 6) { g_sim.gear++; }
    else if (rpm < (brk > 0.0f ? 4800.0f : 3300.0f) && g_sim.gear > 1) { g_sim.gear--; }
    rpm = g_sim.speed / k_kph_per_krpm[g_sim.gear - 1] * 1000.0f;
    if (rpm < 1500.0f) { rpm = 1500.0f; }
    if (rpm > DEMO_MAX_RPM) { rpm = DEMO_MAX_RPM; }

    /* 胎溫：彎道和煞車加熱（左右彎輪流），直線慢慢降 */
    {
        bool  left = (g_sim.seg / 3) & 1;
        float heat_f = d->mode == 1 ? 8.0f : d->mode == 2 ? 6.0f : -2.5f;
        float heat_r = d->mode == 2 ? 4.5f : d->mode == 1 ? 2.0f : -2.0f;

        g_sim.tyre[0] += heat_f * (left ? 0.6f : 1.0f) * dts;
        g_sim.tyre[1] += heat_f * (left ? 1.0f : 0.6f) * dts;
        g_sim.tyre[2] += heat_r * (left ? 0.6f : 1.0f) * dts;
        g_sim.tyre[3] += heat_r * (left ? 1.0f : 0.6f) * dts;
        for (int i = 0; i < 4; i++) {
            if (g_sim.tyre[i] < 70.0f) { g_sim.tyre[i] = 70.0f; }
            if (g_sim.tyre[i] > 112.0f) { g_sim.tyre[i] = 112.0f; }
        }
    }

    g_sim.fuel -= thr / 100.0f * 0.09f * dts;
    if (g_sim.fuel < 0.0f) { g_sim.fuel = 0.0f; }

    sim_fmt_lap(cur, sizeof(cur), now - g_sim.lap_start);
    sim_fmt_lap(last, sizeof(last), g_sim.last_ms);
    sim_fmt_lap(best, sizeof(best), g_sim.best_ms);
    /* 欄位順序見 bridge/README.md 的 DSH1 格式 */
    return snprintf(out, (size_t)cap,
             "DSH1;%lu;1;%d;%d;%d;%d;92;%s;%s;%s;%.1f;%d;%d;%d;%d;%d;%d;%d;%d;%d;%.1f;%.1f;%.1f;%.1f\n",
             (unsigned long)++seq, (int)(g_sim.speed + 0.5f), g_sim.gear, (int)rpm,
             (int)(rpm * 100.0f / DEMO_MAX_RPM), cur, last, best,
             (double)(g_sim.fuel / 4.5f), g_sim.pos, g_sim.lap, DEMO_LAPS, (int)(g_sim.fuel + 0.5f),
             (int)thr, (int)brk, tc_on ? 1 : 0, abs_on ? 1 : 0, g_sim.invalid ? 1 : 0,
             (double)g_sim.tyre[0], (double)g_sim.tyre[1], (double)g_sim.tyre[2], (double)g_sim.tyre[3]);
}
