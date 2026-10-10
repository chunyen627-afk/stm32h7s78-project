/* 六個儀表主題：Classic、GT3、Radar、Retro、Mono、Pocket。
 *
 * 設計出自 caa1211/esp32-gt7-dashboard（MIT License）的同名主題。原作是
 * 2.8 吋 320x240，這裡照 7 吋 800x480 重新排版，配色、元件與資訊配置照原作。
 *
 * 畫圖的方式：大塊的長方形交給 DMA2D（gfx_fill_rect），圓角、圓圈、扇形、
 * 像素字這些形狀由 CPU 一列一列直接寫 framebuffer（橫向模式跟面板 1:1）。
 * CPU 寫之前一定要先 dma2d_wait()，不然還在跑的硬體填色會把它蓋掉。 */
#include "dash_priv.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void dma2d_wait(void);

#define PI_F 3.14159265f

/* ------------------------------------------------------------------ */
/* 繪圖元件                                                             */
/* ------------------------------------------------------------------ */
static void hline(int x0, int x1, int y, uint16_t c)      /* [x0, x1) */
{
    uint16_t *fb = gfx_framebuffer();

    if (y < 0 || y >= H) { return; }
    if (x0 < 0) { x0 = 0; }
    if (x1 > W) { x1 = W; }
    for (int x = x0; x < x1; x++) { fb[y * W + x] = c; }
}

static void rect(int x, int y, int w, int h, int t, uint16_t c)  /* 方角外框 */
{
    gfx_fill_rect(x, y, w, t, c);
    gfx_fill_rect(x, y + h - t, w, t, c);
    gfx_fill_rect(x, y, t, h, c);
    gfx_fill_rect(x + w - t, y, t, h, c);
}

/* 圓角實心方塊。中間交給 DMA2D，上下 r 列的圓角由 CPU 畫。 */
static void rrect_fill(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > h) { r = h / 2; }
    if (r * 2 > w) { r = w / 2; }
    dma2d_wait();
    for (int i = 0; i < r; i++) {
        float dy = (float)r - (float)i - 0.5f;
        int   in = r - (int)(sqrtf((float)(r * r) - dy * dy) + 0.5f);

        hline(x + in, x + w - in, y + i, c);
        hline(x + in, x + w - in, y + h - 1 - i, c);
    }
    gfx_fill_rect(x, y + r, w, h - 2 * r, c);
}

/* 圓角外框：外圈畫 c、內圈用 bg 挖掉。 */
static void rrect(int x, int y, int w, int h, int r, int t, uint16_t c, uint16_t bg)
{
    rrect_fill(x, y, w, h, r, c);
    rrect_fill(x + t, y + t, w - 2 * t, h - 2 * t, (r > t) ? r - t : 0, bg);
}

/* 實心圓、圓環 */
static void disc(int cx, int cy, int r, uint16_t c)
{
    dma2d_wait();
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        hline(cx - dx, cx + dx + 1, cy + dy, c);
    }
}

static void ring(int cx, int cy, int r, int t, uint16_t c)
{
    int ri = r - t;

    dma2d_wait();
    for (int dy = -r; dy <= r; dy++) {
        int xo = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);

        if (dy > -ri && dy < ri) {
            int xi = (int)(sqrtf((float)(ri * ri - dy * dy)) + 0.5f);
            hline(cx - xo, cx - xi, cy + dy, c);
            hline(cx + xi + 1, cx + xo + 1, cy + dy, c);
        } else {
            hline(cx - xo, cx + xo + 1, cy + dy, c);
        }
    }
}

/* 凸四邊形填色（掃描線）。扇形的一格、弧形的一小段都用它畫。 */
static void quad(const float *px, const float *py, uint16_t c)
{
    float ymin = py[0], ymax = py[0];

    for (int i = 1; i < 4; i++) {
        if (py[i] < ymin) { ymin = py[i]; }
        if (py[i] > ymax) { ymax = py[i]; }
    }
    for (int y = (int)ceilf(ymin - 0.5f); y <= (int)floorf(ymax - 0.5f); y++) {
        float yc = (float)y + 0.5f, xl = 1e9f, xr = -1e9f;

        for (int i = 0; i < 4; i++) {
            int   j = (i + 1) & 3;
            float y0 = py[i], y1 = py[j];

            if ((yc >= y0 && yc < y1) || (yc >= y1 && yc < y0)) {
                float x = px[i] + (yc - y0) * (px[j] - px[i]) / (y1 - y0);
                if (x < xl) { xl = x; }
                if (x > xr) { xr = x; }
            }
        }
        if (xl <= xr) { hline((int)(xl + 0.5f), (int)(xr + 0.5f), y, c); }
    }
}

/* 一段環形：角度以度為單位，0 = 右、逆時針為正（螢幕 y 向下，所以算 y 時取負）。 */
static void arc_band(int cx, int cy, int ri, int ro, float a0, float a1, uint16_t c)
{
    int steps = (int)(fabsf(a1 - a0) / 4.0f) + 1;

    dma2d_wait();
    for (int s = 0; s < steps; s++) {
        float t0 = (a0 + (a1 - a0) * s / steps) * PI_F / 180.0f;
        float t1 = (a0 + (a1 - a0) * (s + 1) / steps) * PI_F / 180.0f;
        float px[4] = { cx + ri * cosf(t0), cx + ro * cosf(t0), cx + ro * cosf(t1), cx + ri * cosf(t1) };
        float py[4] = { cy - ri * sinf(t0), cy - ro * sinf(t0), cy - ro * sinf(t1), cy - ri * sinf(t1) };

        quad(px, py, c);
    }
}

/* 像素字（Pocket）：nf_px 每個點放大成 scale x scale 的方塊。大字（檔位、車速）
 * 每格留 1 像素縫，像液晶點陣；小字不留縫，留了筆畫會細得像虛線（實測）。 */
static int px_width(const char *s, int scale)
{
    return nf_width(&nf_px, s) * scale;
}

static void px_text(int x, int y, int scale, const char *s, uint16_t c, int align)
{
    int blk = (scale >= 6) ? scale - 1 : scale;

    if (align == 1) { x -= px_width(s, scale) / 2; }
    if (align == 2) { x -= px_width(s, scale); }
    dma2d_wait();
    for (; *s; s++) {
        const nf_glyph_t *g = 0;

        for (uint16_t i = 0; i < nf_px.count; i++) {
            if (nf_px.glyphs[i].ch == *s) { g = &nf_px.glyphs[i]; break; }
        }
        if (!g) { x += 3 * scale; continue; }
        for (int r = 0; r < (int)nf_px.h; r++) {
            const uint8_t *row = g->data + r * (g->w / 2);

            for (int col = 0; col < (int)g->w; col++) {
                uint8_t a = (col & 1) ? (row[col / 2] & 15) : (row[col / 2] >> 4);

                if (a >= 8) {
                    for (int k = 0; k < blk; k++) {
                        hline(x + col * scale, x + col * scale + blk, y + r * scale + k, c);
                    }
                }
            }
        }
        x += g->w * scale;
    }
}

/* ------------------------------------------------------------------ */
/* 共用的資料格式                                                       */
/* ------------------------------------------------------------------ */
/* 原作的圈速格式：分鐘兩位，沒有就是 --:--.--- */
static void lap2(char *out, int cap, int32_t ms)
{
    if (ms == DSH1_NA) { snprintf(out, (size_t)cap, "--:--.---"); return; }
    snprintf(out, (size_t)cap, "%02ld:%02ld.%03ld", (long)(ms / 60000),
             (long)((ms / 1000) % 60), (long)(ms % 1000));
}

static bool shift_now(const dsh1_t *d)
{
    return d->rpm_pct != DSH1_NA && d->redline_pct != DSH1_NA &&
           d->redline_pct > 0 && d->rpm_pct >= d->redline_pct;
}

static int32_t rpm_pct(const dsh1_t *d)
{
    return (d->rpm_pct == DSH1_NA) ? 0 : d->rpm_pct;
}

static void laps_left(char *out, int cap, const dsh1_t *d)
{
    if (d->lap == DSH1_NA || d->laps_total == DSH1_NA || d->laps_total == 0) {
        snprintf(out, (size_t)cap, "--");
    } else {
        snprintf(out, (size_t)cap, "%ld", (long)(d->laps_total - d->lap));
    }
}

static void tyre_str(char *out, int cap, int32_t x10)
{
    if (x10 == DSH1_NA) { snprintf(out, (size_t)cap, "--"); }
    else { snprintf(out, (size_t)cap, "%ld", (long)(x10 / 10)); }
}

static uint16_t lerp565(uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1, float t)
{
    return RGB565((int)(r0 + (r1 - r0) * t), (int)(g0 + (g1 - g0) * t),
                  (int)(b0 + (b1 - b0) * t));
}

/* ------------------------------------------------------------------ */
/* Classic：黑底、白色圓角框、萊姆綠轉速條、黃色液晶檔位                 */
/* ------------------------------------------------------------------ */
#define CL_BG   RGB565(0, 0, 0)
#define CL_FG   RGB565(240, 240, 240)
#define CL_LIME RGB565(150, 230, 40)
#define CL_GEAR RGB565(250, 220, 20)
#define CL_PUR  RGB565(200, 110, 255)

static void cl_box(int x, int y, int w, int h, const char *lab, int lab_align, uint16_t c)
{
    rrect(x, y, w, h, 10, 2, c, CL_BG);
    if (lab_align == 2) {
        nf_text(&nf_t2, x + w - 12, y + 6, lab, c, CL_BG, 2);
    } else if (lab_align == 1) {
        nf_text(&nf_t2, x + w / 2, y + 6, lab, c, CL_BG, 1);
    } else {
        nf_text(&nf_t2, x + 12, y + 6, lab, c, CL_BG, 0);
    }
}

static void cl_flag(int x, int y, int w, const char *lab, int8_t on, uint16_t col)
{
    cl_box(x, y, w, 76, lab, 1, CL_FG);
    if (on == 1)       { disc(x + w / 2, y + 52, 13, col); }
    else if (on == 0)  { disc(x + w / 2, y + 52, 13, RGB565(150, 150, 150)); }
    else               { ring(x + w / 2, y + 52, 13, 2, RGB565(90, 90, 90)); }
}

void theme_classic(const dsh1_t *d, uint32_t now)
{
    char     buf[24];
    int32_t  pct = rpm_pct(d);
    bool     blink = shift_now(d) && ((now / 80u) & 1u);

    gfx_clear(CL_BG);

    /* 轉速條：外框 + 細直條，紅線位置有紅色標記 */
    rrect(150, 18, 500, 44, 6, 2, CL_FG, CL_BG);
    for (int i = 0; i < 49; i++) {
        bool lit = (i * 100 / 49) < pct;
        gfx_fill_rect(157 + i * 10, 25, 7, 30,
                      lit ? (blink ? C_BLUE : CL_LIME) : RGB565(20, 30, 10));
    }
    if (d->redline_pct != DSH1_NA && d->redline_pct > 0) {
        int   mx = 157 + d->redline_pct * 487 / 100;
        float tx[4] = { (float)mx - 8, (float)mx + 8, (float)mx, (float)mx };
        float ty[4] = { 78, 78, 64, 64 };

        gfx_fill_rect(mx - 1, 14, 3, 52, C_RED);
        dma2d_wait();
        quad(tx, ty, C_RED);
    }

    /* 第一、二列：最佳／上一圈、檔位、差距、踏板 */
    cl_box(30, 92, 240, 84, "Best Lap", 0, CL_FG);
    lap2(buf, sizeof(buf), d->lap_ms[2]);
    nf_text(&nf_s, 42, 128, buf, CL_FG, CL_BG, 0);
    cl_box(30, 184, 240, 84, "Last Lap", 0, CL_FG);
    lap2(buf, sizeof(buf), d->lap_ms[1]);
    nf_text(&nf_s, 42, 220, buf, CL_FG, CL_BG, 0);

    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    nf_text(&nf_gx, 400, 78, buf, CL_GEAR, CL_BG, 1);
    for (int i = 0; i < 4; i++) {
        gfx_fill_rect(333 + (i & 1) * 70, 274 + (i >> 1) * 15, 66, 11,
                      (d->tyre_x10[i] == DSH1_NA) ? RGB565(40, 90, 200) : tyre_color(d->tyre_x10[i]));
    }

    cl_box(530, 92, 240, 84, "Delta", 2, CL_FG);
    {
        uint16_t c = fmt_delta(buf, sizeof(buf), d->lap_ms[1], d->lap_ms[2]);
        nf_text(&nf_s, 758, 128, buf, (c == C_DIM) ? CL_FG : c, CL_BG, 2);
    }
    rrect(530, 184, 240, 44, 6, 2, CL_FG, CL_BG);
    if (d->throttle != DSH1_NA && d->throttle > 0) {
        gfx_fill_rect(534, 188, 232 * d->throttle / 100, 36, RGB565(240, 230, 60));
    }
    rrect(530, 234, 240, 34, 6, 2, CL_FG, CL_BG);
    if (d->brake != DSH1_NA && d->brake > 0) {
        gfx_fill_rect(534, 238, 232 * d->brake / 100, 26, C_RED);
    }

    /* 第三列 */
    cl_box(30, 306, 250, 80, "Current Lap", 0, CL_FG);
    lap2(buf, sizeof(buf), d->lap_ms[0]);
    nf_text(&nf_s, 42, 340, buf, (d->lap_invalid == 1) ? C_RED : CL_FG, CL_BG, 0);
    cl_box(288, 306, 140, 80, "Speed", 0, CL_FG);
    fmt_int(buf, sizeof(buf), d->speed);
    nf_text(&nf_s, 416, 340, buf, CL_FG, CL_BG, 2);
    cl_box(436, 306, 150, 80, "REM", 0, CL_FG);
    fmt_fuel_laps(buf, sizeof(buf), d->fuel_laps_x10);
    nf_text(&nf_s, 574, 340, buf, CL_FG, CL_BG, 2);
    cl_box(594, 306, 176, 80, "POS", 0, CL_FG);
    fmt_int(buf, sizeof(buf), d->position);
    nf_text(&nf_s, 758, 340, buf, CL_FG, CL_BG, 2);

    /* 第四列 */
    cl_flag(30, 394, 140, "ABS", d->abs, C_ORANGE);
    cl_flag(178, 394, 140, "TCS", d->tc, C_RED);
    cl_box(326, 394, 140, 76, "FUEL", 0, CL_PUR);
    fmt_int(buf, sizeof(buf), d->fuel_pct);
    nf_text(&nf_s, 454, 426, buf, CL_FG, CL_BG, 2);
    cl_box(474, 394, 140, 76, "LAP", 0, CL_FG);
    fmt_int(buf, sizeof(buf), d->lap);
    nf_text(&nf_s, 602, 426, buf, CL_FG, CL_BG, 2);
    cl_box(622, 394, 148, 76, "F/ST", 0, CL_FG);
    {
        const char *st = "--";
        if (d->fuel_pct != DSH1_NA) { st = (d->fuel_pct >= 50) ? "HI" : (d->fuel_pct >= 20) ? "MID" : "LO"; }
        nf_text(&nf_t3, 758, 426, st, CL_FG, CL_BG, 2);
    }
}

/* ------------------------------------------------------------------ */
/* GT3：深藍框、漸層轉速條、括號包住的檔位                               */
/* ------------------------------------------------------------------ */
#define G3_BG   RGB565(6, 8, 14)
#define G3_BOX  RGB565(55, 75, 110)
#define G3_LAB  RGB565(150, 172, 205)
#define G3_FG   RGB565(240, 242, 248)
#define G3_POS  RGB565(235, 95, 205)

static uint16_t hue565(float h)          /* h: 0..360，飽和度與亮度固定 */
{
    float s = 0.85f, v = 1.0f, c = v * s;
    float hp = h / 60.0f, x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f)), r, g, b;

    if (hp < 1)      { r = c; g = x; b = 0; }
    else if (hp < 2) { r = x; g = c; b = 0; }
    else if (hp < 3) { r = 0; g = c; b = x; }
    else if (hp < 4) { r = 0; g = x; b = c; }
    else if (hp < 5) { r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }
    return RGB565((int)((r + v - c) * 255), (int)((g + v - c) * 255), (int)((b + v - c) * 255));
}

static void g3_cell(int x, int w, const char *lab)
{
    nf_text(&nf_t2, x + w / 2, 406, lab, G3_LAB, G3_BG, 1);
}

void theme_gt3(const dsh1_t *d, uint32_t now)
{
    char    buf[24];
    int32_t pct = rpm_pct(d);
    bool    blink = shift_now(d) && ((now / 80u) & 1u);

    gfx_clear(G3_BG);

    /* 漸層轉速條：藍 -> 青 -> 綠 -> 黃 -> 橘 -> 紅 */
    for (int i = 0; i < 36; i++) {
        bool lit = (i * 100 / 36) < pct;
        gfx_fill_rect(168 + i * 13, 22, 10, 34,
                      lit ? (blink ? G3_FG : hue565(210.0f - 210.0f * i / 35.0f)) : RGB565(22, 28, 40));
    }
    if (d->redline_pct != DSH1_NA && d->redline_pct > 0) {
        gfx_fill_rect(168 + d->redline_pct * 465 / 100, 14, 3, 50, C_YELLOW);
    }
    if (d->rpm != DSH1_NA) {
        snprintf(buf, sizeof(buf), "%ld RPM", (long)d->rpm);
        nf_text(&nf_t1, 400, 62, buf, G3_LAB, G3_BG, 1);
    }

    /* 左框：車速、差距 */
    rrect(20, 92, 240, 296, 10, 2, G3_BOX, G3_BG);
    nf_text(&nf_t2, 34, 100, "SPEED", G3_LAB, G3_BG, 0);
    fmt_int(buf, sizeof(buf), d->speed);
    nf_text(&nf_l, 246, 128, buf, G3_FG, G3_BG, 2);
    nf_text(&nf_t1, 246, 236, "km/h", G3_LAB, G3_BG, 2);
    gfx_fill_rect(22, 262, 236, 2, G3_BOX);
    nf_text(&nf_t2, 34, 272, "DELTA", G3_LAB, G3_BG, 0);
    {
        uint16_t c = fmt_delta(buf, sizeof(buf), d->lap_ms[1], d->lap_ms[2]);
        nf_text(&nf_m, 140, 314, buf, (c == C_DIM) ? G3_FG : c, G3_BG, 1);
    }

    /* 中央：檔位與兩側括號 */
    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    nf_text(&nf_xl, 400, 98, buf, (d->gear == 'R') ? C_RED : G3_FG, G3_BG, 1);
    arc_band(400, 198, 122, 130, 140.0f, 220.0f, G3_FG);
    arc_band(400, 198, 122, 130, -40.0f, 40.0f, G3_FG);

    /* 檔位下方：煞車條、胎溫四格、油門條 */
    rect(298, 322, 16, 58, 2, G3_LAB);
    if (d->brake != DSH1_NA && d->brake > 0) {
        int h = 54 * d->brake / 100;
        gfx_fill_rect(300, 378 - h, 12, h, C_RED);
    }
    for (int i = 0; i < 4; i++) {
        gfx_fill_rect(330 + (i & 1) * 72, 324 + (i >> 1) * 28, 68, 24,
                      (d->tyre_x10[i] == DSH1_NA) ? RGB565(40, 90, 200) : tyre_color(d->tyre_x10[i]));
    }
    if (d->throttle != DSH1_NA && d->throttle > 0) {
        int h = 58 * d->throttle / 100;
        gfx_fill_rect(486, 380 - h, 16, h, G3_FG);
    }

    /* 右框：最佳、上一圈、目前 */
    rrect(540, 92, 240, 296, 10, 2, G3_BOX, G3_BG);
    nf_text(&nf_t2, 554, 100, "BEST", G3_LAB, G3_BG, 0);
    lap2(buf, sizeof(buf), d->lap_ms[2]);
    nf_text(&nf_s, 766, 130, buf, C_PURPLE, G3_BG, 2);
    nf_text(&nf_t2, 554, 180, "LAST", G3_LAB, G3_BG, 0);
    lap2(buf, sizeof(buf), d->lap_ms[1]);
    nf_text(&nf_s, 766, 210, buf, G3_FG, G3_BG, 2);
    nf_text(&nf_t2, 554, 272, "CURRENT", G3_LAB, G3_BG, 0);
    lap2(buf, sizeof(buf), d->lap_ms[0]);
    nf_text(&nf_s, 766, 314, buf, (d->lap_invalid == 1) ? C_RED : G3_FG, G3_BG, 2);

    /* 底列：FUEL LEFT LAP POS ABS TCS */
    rrect(20, 398, 760, 74, 10, 2, G3_BOX, G3_BG);
    for (int i = 1; i < 6; i++) { gfx_fill_rect(20 + i * 760 / 6, 400, 2, 70, G3_BOX); }
    {
        const int cw = 760 / 6;

        g3_cell(20, cw, "FUEL");
        fmt_int(buf, sizeof(buf), d->fuel_pct);
        nf_text(&nf_s, 20 + cw / 2, 430, buf, G3_FG, G3_BG, 1);
        g3_cell(20 + cw, cw, "LEFT");
        laps_left(buf, sizeof(buf), d);
        nf_text(&nf_s, 20 + cw + cw / 2, 430, buf, G3_FG, G3_BG, 1);
        g3_cell(20 + 2 * cw, cw, "LAP");
        fmt_int(buf, sizeof(buf), d->lap);
        nf_text(&nf_s, 20 + 2 * cw + cw / 2, 430, buf, G3_FG, G3_BG, 1);
        g3_cell(20 + 3 * cw, cw, "POS");
        fmt_int(buf, sizeof(buf), d->position);
        nf_text(&nf_s, 20 + 3 * cw + cw / 2, 430, buf, G3_POS, G3_BG, 1);
        g3_cell(20 + 4 * cw, cw, "ABS");
        ring(20 + 4 * cw + cw / 2, 448, 13, 3, (d->abs == -1) ? G3_BOX : RGB565(60, 220, 90));
        if (d->abs == 1) { disc(20 + 4 * cw + cw / 2, 448, 7, RGB565(60, 220, 90)); }
        g3_cell(20 + 5 * cw, cw, "TCS");
        ring(20 + 5 * cw + cw / 2, 448, 13, 3, (d->tc == -1) ? G3_BOX : RGB565(70, 150, 255));
        if (d->tc == 1) { disc(20 + 5 * cw + cw / 2, 448, 7, RGB565(70, 150, 255)); }
    }
}

/* ------------------------------------------------------------------ */
/* Radar：圓弧轉速環為核心，兩側是遙測資訊                               */
/* ------------------------------------------------------------------ */
#define RD_BG   RGB565(0, 0, 0)
#define RD_BOX  RGB565(60, 66, 76)
#define RD_LAB  RGB565(150, 156, 166)
#define RD_FG   RGB565(240, 240, 240)
#define RD_ORG  RGB565(245, 145, 40)

static void rd_box(int x, int y, int w, int h, const char *lab, const char *val)
{
    rrect(x, y, w, h, 8, 2, RD_BOX, RD_BG);
    nf_text(&nf_t1, x + 10, y + 6, lab, RD_LAB, RD_BG, 0);
    nf_text(&nf_t2, x + w - 10, y + h - 34, val, RD_FG, RD_BG, 2);
}

static uint16_t radar_color(float f)       /* f: 0..1，綠 -> 黃 -> 橘 -> 紅 */
{
    if (f < 0.55f) { return RGB565(70, 205, 70); }
    if (f < 0.75f) { return lerp565(70, 205, 70, 235, 215, 40, (f - 0.55f) / 0.20f); }
    if (f < 0.88f) { return lerp565(235, 215, 40, 245, 140, 30, (f - 0.75f) / 0.13f); }
    return lerp565(245, 140, 30, 230, 45, 40, (f - 0.88f) / 0.12f);
}

void theme_radar(const dsh1_t *d, uint32_t now)
{
    const int   cx = 400, cy = 292, ri = 176, ro = 216, n = 34;
    const float a_start = 210.0f, sweep = 240.0f, span = sweep / n;
    char        buf[24];
    int32_t     pct = rpm_pct(d);
    bool        blink = shift_now(d) && ((now / 80u) & 1u);

    gfx_clear(RD_BG);

    /* 轉速環：34 格，從左下繞過頂端到右下 */
    for (int i = 0; i < n; i++) {
        float f = (float)i / (float)(n - 1);
        float a0 = a_start - i * span, a1 = a0 - span * 0.78f;
        bool  lit = (i * 100 / n) < pct;

        arc_band(cx, cy, ri, ro, a0, a1,
                 lit ? (blink ? C_BLUE : radar_color(f)) : RGB565(38, 42, 38));
    }

    nf_text(&nf_t1, cx, 120, "GEAR", RD_LAB, RD_BG, 1);
    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    nf_text(&nf_gl, cx, 142, buf, (d->gear == 'R') ? C_RED : RD_FG, RD_BG, 1);
    fmt_int(buf, sizeof(buf), d->speed);
    nf_text(&nf_gm, cx, 248, buf, RD_FG, RD_BG, 1);
    nf_text(&nf_t1, cx, 298, "km/h", RD_LAB, RD_BG, 1);
    nf_text(&nf_t1, cx, 322, "RPM", RD_LAB, RD_BG, 1);
    fmt_int(buf, sizeof(buf), d->rpm);
    nf_text(&nf_t3, cx, 340, buf, RD_ORG, RD_BG, 1);

    /* 左欄 */
    fmt_int(buf, sizeof(buf), d->lap);              rd_box(14, 20, 150, 92, "LAP", buf);
    fmt_int(buf, sizeof(buf), d->position);         rd_box(14, 120, 150, 92, "POS", buf);
    lap2(buf, sizeof(buf), d->lap_ms[2]);           rd_box(14, 220, 150, 92, "BEST", buf);
    lap2(buf, sizeof(buf), d->lap_ms[1]);           rd_box(14, 320, 150, 92, "LAST", buf);

    /* 右欄 */
    fmt_int(buf, sizeof(buf), d->fuel_pct);         rd_box(636, 20, 150, 92, "FUEL", buf);
    fmt_fuel_laps(buf, sizeof(buf), d->fuel_laps_x10); rd_box(636, 120, 150, 92, "REM", buf);
    lap2(buf, sizeof(buf), d->lap_ms[0]);           rd_box(636, 220, 150, 92, "CURRENT", buf);
    rrect(636, 320, 150, 92, 8, 2, RD_BOX, RD_BG);
    nf_text(&nf_t1, 646, 326, "PEDALS", RD_LAB, RD_BG, 0);
    nf_text(&nf_t1, 646, 354, "B", RD_LAB, RD_BG, 0);
    hbar(664, 358, 110, 12, d->brake, C_RED);
    nf_text(&nf_t1, 646, 380, "T", RD_LAB, RD_BG, 0);
    hbar(664, 384, 110, 12, d->throttle, RD_FG);

    /* 底列：ABS TCS DELTA 胎溫 */
    rrect(14, 420, 772, 54, 8, 2, RD_BOX, RD_BG);
    nf_text(&nf_t1, 30, 426, "ABS", RD_LAB, RD_BG, 0);
    ring(42, 458, 9, 2, (d->abs == -1) ? RD_BOX : RD_FG);
    if (d->abs == 1) { disc(42, 458, 6, C_ORANGE); }
    nf_text(&nf_t1, 110, 426, "TCS", RD_LAB, RD_BG, 0);
    ring(122, 458, 9, 2, (d->tc == -1) ? RD_BOX : RD_FG);
    if (d->tc == 1) { disc(122, 458, 6, C_YELLOW); }
    nf_text(&nf_t1, 220, 426, "DELTA", RD_LAB, RD_BG, 0);
    {
        uint16_t c = fmt_delta(buf, sizeof(buf), d->lap_ms[1], d->lap_ms[2]);
        nf_text(&nf_t2, 380, 444, buf, (c == C_DIM) ? RD_FG : c, RD_BG, 2);
    }
    nf_text(&nf_t1, 520, 426, "TYRE TEMP", RD_LAB, RD_BG, 0);
    for (int i = 0; i < 4; i++) {
        tyre_str(buf, sizeof(buf), d->tyre_x10[i]);
        nf_text(&nf_t2, 600 + i * 46, 440, buf, RD_FG, RD_BG, 1);
        gfx_fill_rect(584 + i * 46, 466, 32, 4,
                      (d->tyre_x10[i] == DSH1_NA) ? RD_BOX : tyre_color(d->tyre_x10[i]));
    }
}

/* ------------------------------------------------------------------ */
/* Retro：米色紙底、深色字、液晶數字                                     */
/* ------------------------------------------------------------------ */
#define RT_BG   RGB565(236, 229, 212)
#define RT_INK  RGB565(30, 28, 26)
#define RT_DIM  RGB565(120, 112, 100)
#define RT_RED  RGB565(200, 45, 30)

static void rt_cell(int x, int y, int w, int h, const char *lab, const char *val)
{
    rect(x, y, w, h, 2, RT_INK);
    nf_text(&nf_t1, x + 10, y + 6, lab, RT_INK, RT_BG, 0);
    nf_text(&nf_t3, x + w / 2, y + h - 40, val, RT_INK, RT_BG, 1);
}

void theme_retro(const dsh1_t *d, uint32_t now)
{
    char    buf[64];
    int32_t pct = rpm_pct(d);
    bool    blink = shift_now(d) && ((now / 80u) & 1u);

    gfx_clear(RT_BG);

    /* 轉速列 */
    rect(20, 18, 760, 66, 2, RT_INK);
    nf_text(&nf_t2, 32, 22, "RPM", RT_INK, RT_BG, 0);
    fmt_int(buf, sizeof(buf), d->rpm);
    nf_text(&nf_t2, 32, 50, buf, RT_INK, RT_BG, 0);
    for (int i = 0; i < 30; i++) {
        int      x = 150 + i * 20;
        bool     lit = (i * 100 / 30) < pct;
        uint16_t c = (i >= 26) ? RT_RED : RT_INK;

        if (lit) { gfx_fill_rect(x, 40, 16, 34, blink ? RT_RED : c); }
        else     { rect(x, 40, 16, 34, 2, c); }
    }
    if (d->redline_pct != DSH1_NA && d->redline_pct > 0) {
        float mx = 150.0f + d->redline_pct * 600 / 100;
        float tx[4] = { mx - 7, mx + 7, mx, mx };
        float ty[4] = { 24, 24, 36, 36 };

        dma2d_wait();
        quad(tx, ty, RT_RED);
    }

    /* 車速、檔位、圈速 */
    rect(20, 92, 252, 228, 2, RT_INK);
    nf_text(&nf_t2, 32, 98, "SPEED", RT_INK, RT_BG, 0);
    fmt_int(buf, sizeof(buf), d->speed);
    nf_text(&nf_gl, 258, 168, buf, RT_INK, RT_BG, 2);
    nf_text(&nf_t1, 258, 290, "km/h", RT_INK, RT_BG, 2);

    rect(270, 92, 202, 228, 2, RT_INK);
    nf_text(&nf_t2, 371, 98, "GEAR", RT_INK, RT_BG, 1);
    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    nf_text(&nf_gx, 371, 126, buf, (d->gear == 'R') ? RT_RED : RT_INK, RT_BG, 1);

    rect(470, 92, 310, 228, 2, RT_INK);
    nf_text(&nf_t1, 482, 98, "BEST", RT_INK, RT_BG, 0);
    lap2(buf, sizeof(buf), d->lap_ms[2]);
    nf_text(&nf_c3, 768, 118, buf, RT_INK, RT_BG, 2);
    gfx_fill_rect(472, 166, 306, 2, RT_DIM);
    nf_text(&nf_t1, 482, 172, "LAST", RT_INK, RT_BG, 0);
    lap2(buf, sizeof(buf), d->lap_ms[1]);
    nf_text(&nf_c3, 768, 192, buf, RT_INK, RT_BG, 2);
    gfx_fill_rect(472, 240, 306, 2, RT_DIM);
    nf_text(&nf_t1, 482, 246, "CURRENT", RT_INK, RT_BG, 0);
    lap2(buf, sizeof(buf), d->lap_ms[0]);
    nf_text(&nf_c2, 768, 250, buf, (d->lap_invalid == 1) ? RT_RED : RT_INK, RT_BG, 2);
    fmt_delta(buf, sizeof(buf), d->lap_ms[1], d->lap_ms[2]);
    nf_text(&nf_c2, 768, 282, buf, RT_INK, RT_BG, 2);

    /* 油量 剩餘 圈數 名次 */
    fmt_int(buf, sizeof(buf), d->fuel_pct);             rt_cell(20, 318, 192, 70, "FUEL", buf);
    fmt_fuel_laps(buf, sizeof(buf), d->fuel_laps_x10);  rt_cell(210, 318, 192, 70, "REM", buf);
    fmt_int(buf, sizeof(buf), d->lap);                  rt_cell(400, 318, 192, 70, "LAP", buf);
    fmt_int(buf, sizeof(buf), d->position);             rt_cell(590, 318, 190, 70, "POS", buf);

    /* 胎溫、踏板、TC ABS */
    rect(20, 386, 760, 84, 2, RT_INK);
    nf_text(&nf_t1, 32, 392, "TYRE TEMP C", RT_INK, RT_BG, 0);
    {
        char t[4][8];
        for (int i = 0; i < 4; i++) { tyre_str(t[i], sizeof(t[i]), d->tyre_x10[i]); }
        snprintf(buf, sizeof(buf), "FL%s FR%s RL%s RR%s", t[0], t[1], t[2], t[3]);
        nf_text(&nf_c3, 32, 420, buf, RT_INK, RT_BG, 0);
    }
    gfx_fill_rect(470, 388, 2, 80, RT_INK);
    nf_text(&nf_t1, 482, 398, "B", RT_INK, RT_BG, 0);
    rect(500, 398, 120, 18, 2, RT_INK);
    if (d->brake != DSH1_NA && d->brake > 0) { gfx_fill_rect(502, 400, 116 * d->brake / 100, 14, RT_RED); }
    nf_text(&nf_t1, 482, 432, "T", RT_INK, RT_BG, 0);
    rect(500, 432, 120, 18, 2, RT_INK);
    if (d->throttle != DSH1_NA && d->throttle > 0) { gfx_fill_rect(502, 434, 116 * d->throttle / 100, 14, RT_INK); }
    gfx_fill_rect(634, 388, 2, 80, RT_INK);
    snprintf(buf, sizeof(buf), "ABS %s", (d->abs == 1) ? "ON" : (d->abs == 0) ? "--" : "N/A");
    nf_text(&nf_t2, 648, 398, buf, (d->abs == 1) ? RT_RED : RT_INK, RT_BG, 0);
    snprintf(buf, sizeof(buf), "TCS %s", (d->tc == 1) ? "ON" : (d->tc == 0) ? "--" : "N/A");
    nf_text(&nf_t2, 648, 432, buf, (d->tc == 1) ? RT_RED : RT_INK, RT_BG, 0);
}

/* ------------------------------------------------------------------ */
/* Mono：黑底白字、等寬字、分段換檔燈                                    */
/* ------------------------------------------------------------------ */
#define MO_BG   RGB565(0, 0, 0)
#define MO_FG   RGB565(245, 245, 245)
#define MO_DIM  RGB565(110, 110, 110)

static void dotted(int x0, int x1, int y, uint16_t c)
{
    for (int x = x0; x < x1; x += 8) { gfx_fill_rect(x, y, 4, 2, c); }
}

static void mo_pair(int x, int y, int w, const char *lab, const char *val)
{
    nf_text(&nf_c2, x, y, lab, MO_DIM, MO_BG, 0);
    nf_text(&nf_c3, x + w / 2, y + 26, val, MO_FG, MO_BG, 1);
}

void theme_mono(const dsh1_t *d, uint32_t now)
{
    char    buf[64];
    int32_t pct = rpm_pct(d);
    bool    blink = shift_now(d) && ((now / 80u) & 1u);

    gfx_clear(MO_BG);

    /* 分段換檔燈：上方刻度、下方紅線三角 */
    for (int i = 0; i < 45; i++) {
        int  x = 40 + i * 16;
        bool lit = (i * 100 / 45) < pct;

        if (lit && !blink) { gfx_fill_rect(x, 42, 12, 24, MO_FG); }
        else               { rect(x, 42, 12, 24, 1, lit ? MO_DIM : RGB565(70, 70, 70)); }
        if (i % 9 == 0)    { gfx_fill_rect(x + 5, 28, 2, 10, MO_FG); }
    }
    if (d->redline_pct != DSH1_NA && d->redline_pct > 0) {
        float mx = 40.0f + d->redline_pct * 716 / 100;
        float tx[4] = { mx - 7, mx + 7, mx, mx };
        float ty[4] = { 84, 84, 72, 72 };

        dma2d_wait();
        quad(tx, ty, MO_FG);
    }
    dotted(20, 780, 98, MO_DIM);
    gfx_fill_rect(262, 106, 2, 268, MO_DIM);
    gfx_fill_rect(538, 106, 2, 268, MO_DIM);

    /* 左欄 */
    fmt_int(buf, sizeof(buf), d->lap);
    mo_pair(36, 110, 210, "LAP", buf);
    dotted(30, 250, 182, MO_DIM);
    lap2(buf, sizeof(buf), d->lap_ms[2]);
    mo_pair(36, 190, 210, "BEST", buf);
    dotted(30, 250, 262, MO_DIM);
    fmt_delta(buf, sizeof(buf), d->lap_ms[1], d->lap_ms[2]);
    mo_pair(36, 270, 210, "DELTA", buf);

    /* 中央：檔位、車速 */
    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    nf_text(&nf_xl, 400, 100, buf, MO_FG, MO_BG, 1);
    fmt_int(buf, sizeof(buf), d->speed);
    nf_text(&nf_m, 400, 306, buf, MO_FG, MO_BG, 1);
    nf_text(&nf_t1, 400, 358, "km/h", MO_DIM, MO_BG, 1);

    /* 右欄 */
    fmt_int(buf, sizeof(buf), d->position);
    mo_pair(554, 110, 210, "POSITION", buf);
    dotted(550, 770, 182, MO_DIM);
    lap2(buf, sizeof(buf), d->lap_ms[0]);
    mo_pair(554, 190, 210, "CURRENT", buf);
    dotted(550, 770, 262, MO_DIM);
    lap2(buf, sizeof(buf), d->lap_ms[1]);
    mo_pair(554, 270, 210, "LAST", buf);

    /* 底下兩列 */
    gfx_fill_rect(20, 382, 760, 2, MO_DIM);
    nf_text(&nf_c2, 30, 394, "ABS", MO_FG, MO_BG, 0);
    if (d->abs == 1) { gfx_fill_rect(80, 398, 20, 16, MO_FG); } else { rect(80, 398, 20, 16, 2, MO_FG); }
    nf_text(&nf_c2, 116, 394, "TCS", MO_FG, MO_BG, 0);
    if (d->tc == 1) { gfx_fill_rect(166, 398, 20, 16, MO_FG); } else { rect(166, 398, 20, 16, 2, MO_FG); }
    nf_text(&nf_c2, 214, 394, "FUEL", MO_DIM, MO_BG, 0);
    for (int i = 0; i < 10; i++) {
        bool lit = d->fuel_pct != DSH1_NA && (i * 10) < d->fuel_pct;
        if (lit) { gfx_fill_rect(280 + i * 18, 398, 14, 16, MO_FG); }
        else     { rect(280 + i * 18, 398, 14, 16, 1, MO_DIM); }
    }
    {
        char f[8], r[8];
        fmt_int(f, sizeof(f), d->fuel_pct);
        fmt_fuel_laps(r, sizeof(r), d->fuel_laps_x10);
        snprintf(buf, sizeof(buf), "FUEL %s  REM %s", f, r);
        nf_text(&nf_c2, 770, 394, buf, MO_FG, MO_BG, 2);
    }
    {
        char t[4][8];
        for (int i = 0; i < 4; i++) { tyre_str(t[i], sizeof(t[i]), d->tyre_x10[i]); }
        snprintf(buf, sizeof(buf), "TYRE  FL%s FR%s RL%s RR%s", t[0], t[1], t[2], t[3]);
        nf_text(&nf_c2, 30, 436, buf, MO_FG, MO_BG, 0);
    }
    nf_text(&nf_c2, 560, 430, "B", MO_DIM, MO_BG, 0);
    rect(584, 432, 186, 14, 1, MO_FG);
    if (d->brake != DSH1_NA && d->brake > 0) { gfx_fill_rect(586, 434, 182 * d->brake / 100, 10, MO_FG); }
    nf_text(&nf_c2, 560, 452, "T", MO_DIM, MO_BG, 0);
    rect(584, 454, 186, 14, 1, MO_FG);
    if (d->throttle != DSH1_NA && d->throttle > 0) { gfx_fill_rect(586, 456, 182 * d->throttle / 100, 10, MO_FG); }
}

/* ------------------------------------------------------------------ */
/* Pocket：Game Boy 黃綠液晶、像素字                                     */
/* ------------------------------------------------------------------ */
#define PK_BG   RGB565(198, 214, 172)
#define PK_INK  RGB565(38, 54, 40)
#define PK_MID  RGB565(120, 140, 105)

static void dashed(int x0, int x1, int y, uint16_t c)
{
    for (int x = x0; x < x1; x += 16) { gfx_fill_rect(x, y, 10, 3, c); }
}

static void pk_tag(int x, int y, const char *s)        /* 反白的小標籤 */
{
    int w = px_width(s, 3) + 16;

    gfx_fill_rect(x, y, w, 40, PK_INK);
    px_text(x + 8, y + 6, 3, s, PK_BG, 0);
}

void theme_pocket(const dsh1_t *d, uint32_t now)
{
    char    buf[24];
    int32_t pct = rpm_pct(d);
    bool    blink = shift_now(d) && ((now / 80u) & 1u);

    gfx_clear(PK_BG);

    /* 圈數、名次 */
    pk_tag(24, 16, "LAP");
    fmt_int(buf, sizeof(buf), d->lap);
    px_text(260, 16, 4, buf, PK_INK, 1);
    pk_tag(470, 16, "POS");
    fmt_int(buf, sizeof(buf), d->position);
    px_text(700, 16, 4, buf, PK_INK, 1);
    dashed(20, 780, 66, PK_INK);

    /* 大檔位與車速 */
    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    px_text(150, 62, 24, buf, PK_INK, 1);
    fmt_int(buf, sizeof(buf), d->speed);
    px_text(310, 266, 7, buf, PK_INK, 2);
    px_text(326, 300, 3, "KM/H", PK_INK, 0);

    /* 右框：最佳、上一圈、目前 */
    rect(430, 80, 350, 254, 3, PK_INK);
    rect(436, 86, 338, 242, 1, PK_INK);
    px_text(450, 104, 3, "BEST", PK_INK, 0);
    lap2(buf, sizeof(buf), d->lap_ms[2]);
    px_text(764, 104, 3, buf, PK_INK, 2);
    dashed(446, 766, 162, PK_MID);
    px_text(450, 186, 3, "LAST", PK_INK, 0);
    lap2(buf, sizeof(buf), d->lap_ms[1]);
    px_text(764, 186, 3, buf, PK_INK, 2);
    dashed(446, 766, 244, PK_MID);
    px_text(450, 268, 3, "CUR", PK_INK, 0);
    lap2(buf, sizeof(buf), d->lap_ms[0]);
    px_text(764, 268, 3, buf, PK_INK, 2);

    /* 下框：轉速條、差距、油量、ABS、TCS */
    rect(20, 348, 760, 120, 3, PK_INK);
    rect(26, 354, 748, 108, 1, PK_INK);
    px_text(40, 364, 3, "RPM", PK_INK, 0);
    for (int i = 0; i < 22; i++) {
        bool lit = (i * 100 / 22) < pct;
        if (lit && !blink) { gfx_fill_rect(40 + i * 20, 398, 16, 22, PK_INK); }
        else               { rect(40 + i * 20, 398, 16, 22, 2, PK_MID); }
    }
    if (d->redline_pct != DSH1_NA && d->redline_pct > 0) {
        float mx = 40.0f + d->redline_pct * 436 / 100;
        float tx[4] = { mx - 8, mx + 8, mx, mx };
        float ty[4] = { 382, 382, 394, 394 };

        dma2d_wait();
        quad(tx, ty, PK_INK);
    }
    px_text(40, 430, 3, "DELTA", PK_INK, 0);
    fmt_delta(buf, sizeof(buf), d->lap_ms[1], d->lap_ms[2]);
    px_text(500, 430, 3, buf, PK_INK, 2);

    gfx_fill_rect(520, 354, 3, 108, PK_INK);
    fmt_int(buf, sizeof(buf), d->fuel_pct);
    if (d->fuel_pct != DSH1_NA) { strcat(buf, "%"); }
    px_text(570, 368, 2, buf, PK_INK, 1);
    px_text(570, 436, 2, "FUEL", PK_INK, 1);
    gfx_fill_rect(552, 392, 30, 36, PK_INK);                  /* 油桶 */
    gfx_fill_rect(558, 398, 18, 10, PK_BG);
    gfx_fill_rect(582, 398, 6, 24, PK_INK);

    gfx_fill_rect(620, 354, 3, 108, PK_INK);
    ring(670, 404, 30, 3, PK_INK);
    if (d->abs == 1) { disc(670, 404, 26, PK_INK); }
    px_text(670, 396, 2, "ABS", (d->abs == 1) ? PK_BG : PK_INK, 1);
    px_text(670, 440, 2, "ABS", PK_INK, 1);

    gfx_fill_rect(706, 354, 3, 108, PK_INK);
    if (d->tc == 1) { gfx_fill_rect(718, 380, 50, 44, PK_INK); }
    else            { rect(718, 380, 50, 44, 3, PK_INK); }
    px_text(743, 396, 2, "TC", (d->tc == 1) ? PK_BG : PK_INK, 1);
    px_text(743, 440, 2, "TCS", PK_INK, 1);
}
