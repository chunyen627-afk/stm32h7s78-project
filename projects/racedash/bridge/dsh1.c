/* 組 DSH1 一行（板子上的原作解析就是看這個格式，詳見 README.md「DSH1 格式」）：
 *   DSH1;序號;遊戲在跑;速度;檔位;轉速;轉速%;紅線%;本圈;上一圈;最佳圈;剩幾圈油;名次;
 *   第幾圈;共幾圈;油量%;油門%;煞車%;TC;ABS;本圈無效;胎溫左前;右前;左後;右後
 * 不知道的欄位一律 "--"。 */
#include "bridge.h"

#include <stdio.h>
#include <string.h>

void tele_clear(tele_t *t)
{
    memset(t, 0, sizeof(*t));
    t->gear = TELE_UNKNOWN_I;
    t->cur_ms = t->last_ms = t->best_ms = TELE_UNKNOWN_I;
    t->lap = t->total_laps = t->position = TELE_UNKNOWN_I;
    t->tc = t->abs = t->invalid = TELE_UNKNOWN_I;
    t->speed_kmh = t->rpm = t->max_rpm = t->throttle = t->brake = NAN;
    t->fuel_l = t->fuel_max_l = t->fuel_laps = t->fuel_pct = NAN;
    for (int i = 0; i < 4; i++) { t->tyre_c[i] = NAN; }
}

/* 數值：範圍外或不知道就 "--"（JS 的 numeric(v, min, max, digits)） */
static int num(char *o, int cap, double v, double lo, double hi, int digits)
{
    if (isnan(v) || v < lo || v > hi) { return snprintf(o, cap, "--"); }
    return snprintf(o, cap, "%.*f", digits, v);
}

static int inum(char *o, int cap, int v, int lo, int hi)
{
    if (v == TELE_UNKNOWN_I || v < lo || v > hi) { return snprintf(o, cap, "--"); }
    return snprintf(o, cap, "%d", v);
}

/* 圈速 mm:ss.mmm（分鐘至少兩位）；沒有就 "--"（JS 的 lap()） */
static int laptime(char *o, int cap, int32_t ms)
{
    if (ms == TELE_UNKNOWN_I || ms <= 0 || ms >= 1000 * 60 * 1000) { return snprintf(o, cap, "--"); }
    return snprintf(o, cap, "%02d:%02d.%03d", (int)(ms / 60000), (int)((ms / 1000) % 60), (int)(ms % 1000));
}

static int flag(char *o, int cap, int v)
{
    if (v == TELE_UNKNOWN_I) { return snprintf(o, cap, "--"); }
    return snprintf(o, cap, "%d", v ? 1 : 0);
}

int dsh1_format(char *out, int cap, uint32_t seq, bool game_running, const tele_t *t, float fuel_laps_est)
{
    char  f[25][24];
    float fuel_pct = t->fuel_pct;

    if (isnan(fuel_pct) && !isnan(t->fuel_l) && !isnan(t->fuel_max_l) && t->fuel_max_l > 0) {
        fuel_pct = t->fuel_l / t->fuel_max_l * 100.0f;
    }
    snprintf(f[0], 24, "DSH1");
    snprintf(f[1], 24, "%lu", (unsigned long)seq);
    snprintf(f[2], 24, "%d", game_running ? 1 : 0);
    num(f[3], 24, t->speed_kmh, 0, 1500, 0);
    if (t->gear == TELE_UNKNOWN_I) { snprintf(f[4], 24, "--"); }
    else if (t->gear < 0)  { snprintf(f[4], 24, "R"); }
    else if (t->gear == 0) { snprintf(f[4], 24, "N"); }
    else if (t->gear <= 9) { snprintf(f[4], 24, "%d", t->gear); }
    else { snprintf(f[4], 24, "--"); }
    num(f[5], 24, t->rpm, 0, 30000, 0);
    num(f[6], 24, (t->max_rpm > 0) ? t->rpm / t->max_rpm * 100.0 : NAN, 0, 100, 0);
    snprintf(f[7], 24, "92");                       /* 紅線（換檔燈）：轉速 92% 起亮 */
    laptime(f[8], 24, t->cur_ms);
    laptime(f[9], 24, t->last_ms);
    laptime(f[10], 24, t->best_ms);
    num(f[11], 24, !isnan(t->fuel_laps) ? t->fuel_laps : fuel_laps_est, 0, 9999, 1);
    /* 名次、圈數、總圈數是 0 就是「沒有」（例如 EVO 單人練習名次報 0、計時賽總圈數 0） */
    inum(f[12], 24, t->position, 1, 9999);
    inum(f[13], 24, t->lap, 1, 9999);
    inum(f[14], 24, t->total_laps, 1, 9999);
    num(f[15], 24, fuel_pct, 0, 100, 0);
    num(f[16], 24, t->throttle * 100.0, 0, 100, 0);
    num(f[17], 24, t->brake * 100.0, 0, 100, 0);
    flag(f[18], 24, t->tc);
    flag(f[19], 24, t->abs);
    flag(f[20], 24, t->invalid);
    for (int i = 0; i < 4; i++) { num(f[21 + i], 24, t->tyre_c[i], -100, 1000, 1); }

    int n = 0;
    for (int i = 0; i < 25 && n < cap; i++) {
        n += snprintf(out + n, cap - n, i ? ";%s" : "%s", f[i]);
    }
    if (n < cap - 1) { out[n++] = '\n'; out[n] = 0; }
    return n;
}
