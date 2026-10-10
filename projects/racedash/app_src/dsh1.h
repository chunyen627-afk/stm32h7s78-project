#ifndef DSH1_H
#define DSH1_H

#include <stdbool.h>
#include <stdint.h>

/* SimHub 送來的一行遙測（DSH1 格式，25 個以分號分隔的欄位）。
 * 格式沿用 caa1211/esp32-gt7-dashboard（MIT），公式在 simhub/ 底下。
 *
 * 每個欄位都可能是 "--"（遊戲沒提供），所以每個值都有自己的「有沒有」：
 * 數值欄用 DSH1_NA 表示沒有，旗標用 -1。 */
#define DSH1_NA   (-2147483647 - 1)

typedef struct {
    uint32_t seq;            /* 每次公式執行就變，用來判斷資料還活著 */
    bool     running;        /* 遊戲在跑 */
    int32_t  speed;          /* km/h */
    char     gear;           /* 'N' 'R' '1'..'9'，0 = 沒有 */
    int32_t  rpm;
    int32_t  rpm_pct;        /* 0..100，轉速條 */
    int32_t  redline_pct;    /* 0..100，0 = 不提示換檔 */
    int32_t  lap_ms[3];      /* 目前／上一圈／最佳，毫秒 */
    int32_t  fuel_laps_x10;  /* 剩餘油量還能跑幾圈 x10 */
    int32_t  position;
    int32_t  lap, laps_total;
    int32_t  fuel_pct;
    int32_t  throttle, brake;
    int8_t   tc, abs, lap_invalid;   /* 0 / 1，-1 = 沒有 */
    int32_t  tyre_x10[4];    /* 胎溫 x10：左前 右前 左後 右後 */
} dsh1_t;

/* 成功回 true 並填好 out；格式或範圍不對就回 false，out 不動 ——
 * 壞掉的一行不能把畫面上的好資料蓋掉。line 會被改寫（切欄位）。 */
bool dsh1_parse(char *line, dsh1_t *out);

#endif
