/* dash.c 與 themes.c 共用的繪圖小工具（只給儀表畫面內部用）。 */
#ifndef DASH_PRIV_H
#define DASH_PRIV_H

#include <stdint.h>
#include "gfx.h"
#include "numfont.h"
#include "dsh1.h"

#define W 800
#define H 480

#define C_BG      RGB565(  0,   0,   0)
#define C_PANEL   RGB565( 22,  24,  30)
#define C_LINE    RGB565( 55,  60,  70)
#define C_TEXT    RGB565(235, 235, 240)
#define C_DIM     RGB565(120, 125, 135)
#define C_GREEN   RGB565( 40, 210,  80)
#define C_YELLOW  RGB565(250, 200,  30)
#define C_RED     RGB565(240,  45,  45)
#define C_BLUE    RGB565( 50, 130, 255)
#define C_ORANGE  RGB565(255, 140,   0)
#define C_PURPLE  RGB565(190,  90, 255)
#define C_OFF     RGB565( 35,  38,  45)   /* 沒亮的燈、長條的底槽 */

/* 平滑字：y 是格子上緣；align 0 左、1 置中、2 右對齊 x。
 * 抗鋸齒跟 bg 混色（不讀 framebuffer），所以字一定要畫在純色的底上。 */
int      nf_width(const nf_font_t *f, const char *s);
void     nf_text(const nf_font_t *f, int x, int y, const char *s,
                 uint16_t fg, uint16_t bg, int align);
uint16_t mix565(uint16_t bg, uint16_t fg, int a /* 0..15 */);

void     fmt_lap(char *out, int cap, int32_t ms);
void     fmt_int(char *out, int cap, int32_t v);
void     fmt_fuel_laps(char *out, int cap, int32_t x10);
uint16_t fmt_delta(char *out, int cap, int32_t last, int32_t best);
void     fmt_laps(char *out, int cap, const dsh1_t *d);

uint16_t tyre_color(int32_t t_x10);
void     hbar(int x, int y, int w, int h, int32_t pct, uint16_t col);
void     vbar(int x, int y, int w, int h, int32_t pct, uint16_t col);

/* themes.c：原專案（caa1211/esp32-gt7-dashboard）的六個主題 */
void theme_classic(const dsh1_t *d, uint32_t now);
void theme_gt3(const dsh1_t *d, uint32_t now);
void theme_radar(const dsh1_t *d, uint32_t now);
void theme_retro(const dsh1_t *d, uint32_t now);
void theme_mono(const dsh1_t *d, uint32_t now);
void theme_pocket(const dsh1_t *d, uint32_t now);

#endif
