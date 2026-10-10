/* DSH1 解析。規則照原專案的 docs/TELEMETRY_PROTOCOL.md：
 * 剛好 25 欄、第 0 欄是 "DSH1"、第 2 欄（遊戲在跑）必須是 0/1，
 * 其餘欄可以是 "--"；空字串、非數字、超出範圍一律判整行無效。 */
#include "dsh1.h"
#include <string.h>

#define FIELDS 25

/* 解析 "-12.3" 這種最多一位小數的數字，回傳 x10。用整數做，不帶 float。 */
static bool num_x10(const char *s, int32_t lo, int32_t hi, int32_t *out)
{
    bool    neg = false;
    int32_t v = 0, frac = 0;
    int     digits = 0, fdigits = 0;

    if (strcmp(s, "--") == 0) { *out = DSH1_NA; return true; }
    if (*s == '-') { neg = true; s++; }
    while (*s >= '0' && *s <= '9') {
        if (v > 100000) { return false; }
        v = v * 10 + (*s++ - '0');
        digits++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (fdigits == 0) { frac = *s - '0'; }   /* 只留第一位小數 */
            fdigits++;
            s++;
        }
        if (fdigits == 0) { return false; }
    }
    if (digits == 0 || *s != '\0') { return false; }
    v = v * 10 + frac;
    if (neg) { v = -v; }
    if (v < lo * 10 || v > hi * 10) { return false; }
    *out = v;
    return true;
}

static bool num(const char *s, int32_t lo, int32_t hi, int32_t *out)
{
    int32_t v;

    if (!num_x10(s, lo, hi, &v)) { return false; }
    *out = (v == DSH1_NA) ? DSH1_NA : v / 10;
    return true;
}

static bool flag(const char *s, int8_t *out)
{
    if (strcmp(s, "--") == 0) { *out = -1; return true; }
    if (strcmp(s, "0") == 0)  { *out = 0;  return true; }
    if (strcmp(s, "1") == 0)  { *out = 1;  return true; }
    return false;
}

/* "mm:ss.fff"，分鐘可以到 999。 */
static bool lap(const char *s, int32_t *out)
{
    int32_t m = 0, sec, ms;
    int     md = 0;

    if (strcmp(s, "--") == 0) { *out = DSH1_NA; return true; }
    while (*s >= '0' && *s <= '9') { m = m * 10 + (*s++ - '0'); md++; }
    if (md < 1 || md > 3 || *s++ != ':') { return false; }
    if (!(s[0] >= '0' && s[0] <= '5' && s[1] >= '0' && s[1] <= '9' && s[2] == '.')) { return false; }
    sec = (s[0] - '0') * 10 + (s[1] - '0');
    s += 3;
    if (!(s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' &&
          s[2] >= '0' && s[2] <= '9' && s[3] == '\0')) { return false; }
    ms = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
    *out = (m * 60 + sec) * 1000 + ms;
    return true;
}

bool dsh1_parse(char *line, dsh1_t *out)
{
    char   *f[FIELDS];
    int     n = 0;
    char   *p = line;
    dsh1_t  d;

    /* 切欄位。不用 strtok：它會把連續的分號當成一個，空欄位就消失了。 */
    f[n++] = p;
    while (*p) {
        if (*p == ';') {
            *p = '\0';
            if (n == FIELDS) { return false; }      /* 欄位太多 */
            f[n++] = p + 1;
        }
        p++;
    }
    if (n != FIELDS || strcmp(f[0], "DSH1") != 0) { return false; }

    memset(&d, 0, sizeof(d));

    /* 序號是 uint32，可能超過 int32 —— 自己解。 */
    {
        const char *s = f[1];
        uint32_t    v = 0;

        if (*s == '\0') { return false; }
        while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s++ - '0'); }
        if (*s != '\0') { return false; }
        d.seq = v;
    }
    if (strcmp(f[2], "1") == 0)      { d.running = true; }
    else if (strcmp(f[2], "0") != 0) { return false; }

    if (!num(f[3], 0, 1500, &d.speed)) { return false; }

    if (strcmp(f[4], "--") == 0) {
        d.gear = 0;
    } else if (f[4][1] == '\0' &&
               (f[4][0] == 'N' || f[4][0] == 'R' || (f[4][0] >= '1' && f[4][0] <= '9'))) {
        d.gear = f[4][0];
    } else {
        return false;
    }

    if (!num(f[5], 0, 30000, &d.rpm))          { return false; }
    if (!num(f[6], 0, 100, &d.rpm_pct))        { return false; }
    if (!num(f[7], 0, 100, &d.redline_pct))    { return false; }
    for (int i = 0; i < 3; i++) {
        if (!lap(f[8 + i], &d.lap_ms[i]))      { return false; }
    }
    if (!num_x10(f[11], 0, 9999, &d.fuel_laps_x10)) { return false; }
    if (!num(f[12], 0, 9999, &d.position))     { return false; }
    if (!num(f[13], 0, 9999, &d.lap))          { return false; }
    if (!num(f[14], 0, 9999, &d.laps_total))   { return false; }
    if (!num(f[15], 0, 100, &d.fuel_pct))      { return false; }
    if (!num(f[16], 0, 100, &d.throttle))      { return false; }
    if (!num(f[17], 0, 100, &d.brake))         { return false; }
    if (!flag(f[18], &d.tc))                   { return false; }
    if (!flag(f[19], &d.abs))                  { return false; }
    if (!flag(f[20], &d.lap_invalid))          { return false; }
    for (int i = 0; i < 4; i++) {
        if (!num_x10(f[21 + i], -100, 1000, &d.tyre_x10[i])) { return false; }
    }

    *out = d;
    return true;
}
