/* 原作儀表（C++，port.cpp）對 C 主程式的介面。 */
#ifndef GT7DASH_H_INCLUDED
#define GT7DASH_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#define GT7DASH_W 320
#define GT7DASH_H 240

void gt7dash_init(void);
/* 交一行 DSH1 給原作解析（不含換行）。回 1 = 格式正確。 */
int  gt7dash_feed(const char *line);
/* 跑一次原作的主迴圈：選資料來源、處理觸控、畫有變的部分。 */
void gt7dash_step(void);
/* 320x240 的畫布，16 位元，位元組順序是 LovyanGFX 的 swap565（高位元組在前）。 */
const uint16_t *gt7dash_canvas(void);
/* 同一個畫面的 2 倍抗鋸齒版本（640x480，同樣是 swap565），見 hires/hires.cpp。
 * 記憶體不夠配不出來時回 NULL，那就用上面那張放大。 */
const uint16_t *gt7dash_canvas_hi(void);
/* 上次呼叫之後 2 倍畫布改過的列 [y0, y1)。沒改過回 0。 */
int gt7dash_canvas_hi_dirty(int *y0, int *y1);
/* gt7dash_init() 之前呼叫：要不要配 2 倍鏡像（預設要）。 */
void gt7dash_hires_set(int on);

/* ---- 由 C 這邊（race_main.c）提供 ---- */
/* 觸控：座標已換算成畫布的 320x240。沒按回 false。 */
bool racedash_touch(int *x, int *y);
/* 原作調背光（0~255）。 */
void racedash_backlight(unsigned char brightness);
/* 原作的設定（ESP32 NVS 的替代）：主題、亮度、觸控方向、連線方式。 */
int  racedash_pref_get(const char *key, unsigned char *value);
void racedash_pref_put(const char *key, unsigned char value);
void racedash_pref_clear(void);

#endif
