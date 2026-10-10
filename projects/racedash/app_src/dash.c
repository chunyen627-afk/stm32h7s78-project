/* 賽車儀表的畫面（800x480 橫向）。
 *
 * 版面參考 caa1211/esp32-gt7-dashboard 的 GT3 主題（MIT），但那邊是 2.8 吋
 * 320x240，這裡照 7 吋重新排：上方整條換檔燈、中央大檔位、左邊車速、
 * 右邊圈速、下方油量／圈數／踏板／TC ABS／胎溫。
 *
 * 大數字是 Bahnschrift 預先渲染的平滑字（tools/gennum.py），標籤用 24px 中文字型。缺的值（遊戲沒提供）一律畫成 "--"。 */
#include "dash.h"
#include "dash_priv.h"
#include <stdio.h>
#include <string.h>


/* ------------------------------------------------------------------ */
/* 大數字：Bahnschrift 預先渲染的 4 bit 灰度字（core/numfont.c）          */
/* ------------------------------------------------------------------ */
/* 抗鋸齒要跟背景混色。這裡不讀 framebuffer（讀 PSRAM 慢），而是由呼叫端
 * 告訴我們底下是什麼顏色 —— 儀表的數字都畫在純色的底上。 */
void dma2d_wait(void);

static const nf_glyph_t *nf_find(const nf_font_t *f, char c)
{
    for (uint16_t i = 0; i < f->count; i++) {
        if (f->glyphs[i].ch == c) { return &f->glyphs[i]; }
    }
    return 0;
}

int nf_width(const nf_font_t *f, const char *s)
{
    int w = 0;

    for (; *s; s++) {
        const nf_glyph_t *g = nf_find(f, *s);
        w += g ? g->w : f->h / 4;          /* 缺字當空白 */
    }
    return w;
}

uint16_t mix565(uint16_t bg, uint16_t fg, int a /* 0..15 */)
{
    int r = ((bg >> 11) * (15 - a) + (fg >> 11) * a) / 15;
    int g = (((bg >> 5) & 63) * (15 - a) + ((fg >> 5) & 63) * a) / 15;
    int b = ((bg & 31) * (15 - a) + (fg & 31) * a) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* align: 0 = 左對齊 x、1 = 置中於 x、2 = 右對齊 x；y 是字的上緣。 */
void nf_text(const nf_font_t *f, int x, int y, const char *s,
                    uint16_t fg, uint16_t bg, int align)
{
    uint16_t  lut[16];
    uint16_t *fb = gfx_framebuffer();
    int       w = nf_width(f, s);

    if (align == 1) { x -= w / 2; }
    if (align == 2) { x -= w; }
    for (int a = 0; a < 16; a++) { lut[a] = mix565(bg, fg, a); }

    dma2d_wait();      /* 先前排進去的硬體填色（清畫面）要先落地，不然會蓋掉字 */

    for (; *s; s++) {
        const nf_glyph_t *g = nf_find(f, *s);

        if (!g) { x += f->h / 4; continue; }
        for (int r = 0; r < (int)f->h; r++) {
            int            py = y + r;
            const uint8_t *row = g->data + r * (g->w / 2);

            if (py < 0 || py >= H) { continue; }
            for (int c = 0; c < (int)g->w; c += 2) {
                uint8_t b = row[c / 2];
                int     px = x + c;

                if (b == 0) { continue; }
                /* 橫向模式跟面板 1:1，直接寫 framebuffer（gfx.h 的說明） */
                if ((b >> 4) && px >= 0 && px < W)         { fb[py * W + px]     = lut[b >> 4]; }
                if ((b & 15) && px + 1 >= 0 && px + 1 < W) { fb[py * W + px + 1] = lut[b & 15]; }
            }
        }
        x += g->w;
    }
}

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */
void fmt_lap(char *out, int cap, int32_t ms)
{
    if (ms == DSH1_NA) { snprintf(out, (size_t)cap, "-:--.---"); return; }
    snprintf(out, (size_t)cap, "%ld:%02ld.%03ld", (long)(ms / 60000),
             (long)((ms / 1000) % 60), (long)(ms % 1000));
}

void fmt_int(char *out, int cap, int32_t v)
{
    if (v == DSH1_NA) { snprintf(out, (size_t)cap, "--"); return; }
    snprintf(out, (size_t)cap, "%ld", (long)v);
}

static void label(int x, int y, const char *s)
{
    gfx_text(x, y, s, C_DIM);
}

/* 水平長條：底槽 + 百分比填色 */
void hbar(int x, int y, int w, int h, int32_t pct, uint16_t col)
{
    gfx_fill_rect(x, y, w, h, C_OFF);
    if (pct != DSH1_NA && pct > 0) {
        gfx_fill_rect(x, y, w * (pct > 100 ? 100 : pct) / 100, h, col);
    }
}

/* 垂直長條（踏板），由下往上長 */
void vbar(int x, int y, int w, int h, int32_t pct, uint16_t col)
{
    gfx_fill_rect(x, y, w, h, C_OFF);
    if (pct != DSH1_NA && pct > 0) {
        int fh = h * (pct > 100 ? 100 : pct) / 100;
        gfx_fill_rect(x, y + h - fh, w, fh, col);
    }
}

/* 胎溫的顏色。各遊戲的「適溫」不同，這裡用常見的乾地胎區間：
 * 太冷藍、適溫綠、偏熱黃、過熱紅。 */
uint16_t tyre_color(int32_t t_x10)
{
    if (t_x10 == DSH1_NA) { return C_OFF; }
    if (t_x10 < 600)  { return C_BLUE; }
    if (t_x10 < 1000) { return C_GREEN; }
    if (t_x10 < 1100) { return C_YELLOW; }
    return C_RED;
}

/* ------------------------------------------------------------------ */
/* 儀表                                                                 */
/* ------------------------------------------------------------------ */
#define SHIFT_SEGS  20

static void draw_shift_lights(const dsh1_t *d, uint32_t now)
{
    const int y = 8, h = 44, gap = 6;
    const int sw = (W - 16 - gap * (SHIFT_SEGS - 1)) / SHIFT_SEGS;
    int32_t   pct = (d->rpm_pct == DSH1_NA) ? 0 : d->rpm_pct;
    int       lit = (int)(pct * SHIFT_SEGS / 100);
    bool      shift = d->redline_pct != DSH1_NA && d->redline_pct > 0 &&
                      pct >= d->redline_pct;
    bool      blink = shift && ((now / 80u) & 1u);

    for (int i = 0; i < SHIFT_SEGS; i++) {
        int      x = 8 + i * (sw + gap);
        uint16_t c;

        if (shift) {
            c = blink ? C_BLUE : C_OFF;        /* 該換檔了：整條閃 */
        } else if (i >= lit) {
            c = C_OFF;
        } else if (i < 12) {
            c = C_GREEN;
        } else if (i < 17) {
            c = C_YELLOW;
        } else {
            c = C_RED;
        }
        gfx_fill_rect(x, y, sw, h, c);
    }
}

static void draw_indicator(int x, int y, const char *txt, int8_t on, uint16_t col)
{
    uint16_t bg = (on == 1) ? col : C_PANEL;
    uint16_t fg = (on == 1) ? C_BG : (on == -1 ? C_LINE : C_DIM);

    gfx_fill_rect(x, y, 90, 44, bg);
    gfx_text_center(x + 45, y + 10, txt, fg);
}

/* 完整：什麼都有。 */
static void page_full(const dsh1_t *d, uint32_t now)
{
    char buf[24];

    gfx_clear(C_BG);
    draw_shift_lights(d, now);

    /* --- 中央：檔位 --- */
    gfx_fill_rect(300, 68, 200, 250, C_PANEL);
    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    nf_text(&nf_xl, 400, 93, buf, (d->gear == 'R') ? C_RED : C_TEXT, C_PANEL, 1);

    /* --- 左：車速、轉速 --- */
    label(24, 72, "速度");
    fmt_int(buf, sizeof(buf), d->speed);
    nf_text(&nf_l, 280, 104, buf, C_TEXT, C_BG, 2);
    gfx_text(208, 222, "km/h", C_DIM);

    label(24, 262, "轉速");
    fmt_int(buf, sizeof(buf), d->rpm);
    nf_text(&nf_s, 280, 266, buf, C_TEXT, C_BG, 2);

    /* --- 右：圈速 --- */
    {
        static const char *const names[3] = { "目前", "上一圈", "最佳" };
        static const int ys[3] = { 72, 160, 238 };
        static const nf_font_t *const fs[3] = { &nf_m, &nf_s, &nf_s };

        for (int i = 0; i < 3; i++) {
            uint16_t c = C_TEXT;

            if (i == 0 && d->lap_invalid == 1) { c = C_RED; }   /* 這圈不算 */
            if (i == 2) { c = C_PURPLE; }
            label(524, ys[i], names[i]);
            fmt_lap(buf, sizeof(buf), d->lap_ms[i]);
            nf_text(fs[i], 780, ys[i] + 28, buf, c, C_BG, 2);
        }
    }

    gfx_fill_rect(0, 330, W, 2, C_LINE);

    /* --- 下：油門／煞車 --- */
    vbar(24, 346, 34, 120, d->throttle, C_GREEN);
    vbar(66, 346, 34, 120, d->brake, C_RED);

    /* --- 油量 --- */
    label(124, 346, "油量");
    fmt_int(buf, sizeof(buf), d->fuel_pct);
    if (d->fuel_pct != DSH1_NA) { strcat(buf, "%"); }
    nf_text(&nf_s, 360, 348, buf, C_TEXT, C_BG, 2);
    hbar(124, 396, 236, 14, d->fuel_pct,
         (d->fuel_pct != DSH1_NA && d->fuel_pct < 15) ? C_RED : C_ORANGE);
    label(124, 424, "可跑");
    if (d->fuel_laps_x10 == DSH1_NA) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%ld.%ld", (long)(d->fuel_laps_x10 / 10),
                 (long)(d->fuel_laps_x10 % 10));
    }
    nf_text(&nf_xs, 330, 426, buf, C_TEXT, C_BG, 2);
    gfx_text(336, 428, "圈", C_DIM);

    /* --- 圈數、名次 --- */
    label(390, 346, "圈數");
    if (d->lap == DSH1_NA) {
        snprintf(buf, sizeof(buf), "--");
    } else if (d->laps_total == DSH1_NA || d->laps_total == 0) {
        snprintf(buf, sizeof(buf), "%ld", (long)d->lap);
    } else {
        snprintf(buf, sizeof(buf), "%ld/%ld", (long)d->lap, (long)d->laps_total);
    }
    nf_text(&nf_s, 390, 378, buf, C_TEXT, C_BG, 0);
    label(390, 424, "名次");
    fmt_int(buf, sizeof(buf), d->position);
    nf_text(&nf_s, 460, 420, buf, C_YELLOW, C_BG, 0);

    /* --- TC / ABS --- */
    draw_indicator(540, 350, "TC", d->tc, C_YELLOW);
    draw_indicator(540, 410, "ABS", d->abs, C_ORANGE);

    /* --- 胎溫（左前 右前 / 左後 右後）--- */
    label(650, 340, "胎溫");
    for (int i = 0; i < 4; i++) {
        int x = 650 + (i & 1) * 72, y = 368 + (i >> 1) * 56;

        gfx_fill_rect(x, y, 64, 48, tyre_color(d->tyre_x10[i]));
        if (d->tyre_x10[i] == DSH1_NA) {
            gfx_text_center(x + 32, y + 12, "--", C_DIM);
        } else {
            snprintf(buf, sizeof(buf), "%ld", (long)(d->tyre_x10[i] / 10));
            gfx_text_center(x + 32, y + 12, buf, C_BG);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 其他儀表頁（點一下螢幕切換，見 race_main.c）                        */
/* ------------------------------------------------------------------ */
void fmt_fuel_laps(char *out, int cap, int32_t x10)
{
    if (x10 == DSH1_NA) { snprintf(out, (size_t)cap, "--"); return; }
    snprintf(out, (size_t)cap, "%ld.%ld", (long)(x10 / 10), (long)(x10 % 10));
}

/* 上一圈跟最佳圈的差距（秒，三位小數）。慢了是正的、快了是負的。 */
uint16_t fmt_delta(char *out, int cap, int32_t last, int32_t best)
{
    int32_t d;

    if (last == DSH1_NA || best == DSH1_NA || best == 0) {
        snprintf(out, (size_t)cap, "--");
        return C_DIM;
    }
    d = last - best;
    snprintf(out, (size_t)cap, "%c%ld.%03ld", d < 0 ? '-' : '+',
             (long)((d < 0 ? -d : d) / 1000), (long)((d < 0 ? -d : d) % 1000));
    return (d <= 0) ? C_GREEN : C_RED;
}

void fmt_laps(char *out, int cap, const dsh1_t *d)
{
    if (d->lap == DSH1_NA) {
        snprintf(out, (size_t)cap, "--");
    } else if (d->laps_total == DSH1_NA || d->laps_total == 0) {
        snprintf(out, (size_t)cap, "%ld", (long)d->lap);
    } else {
        snprintf(out, (size_t)cap, "%ld/%ld", (long)d->lap, (long)d->laps_total);
    }
}

/* 大字：開車時瞄一眼用。只有換檔燈、超大檔位、車速、轉速、踏板。 */
static void page_big(const dsh1_t *d, uint32_t now)
{
    char buf[24];

    gfx_clear(C_BG);
    draw_shift_lights(d, now);

    gfx_fill_rect(250, 76, 300, 300, C_PANEL);
    buf[0] = d->gear ? d->gear : '-';
    buf[1] = '\0';
    nf_text(&nf_xl, 400, 126, buf, (d->gear == 'R') ? C_RED : C_TEXT, C_PANEL, 1);

    label(24, 96, "速度");
    fmt_int(buf, sizeof(buf), d->speed);
    nf_text(&nf_l, 230, 160, buf, C_TEXT, C_BG, 2);
    gfx_text(166, 276, "km/h", C_DIM);

    label(580, 96, "轉速");
    fmt_int(buf, sizeof(buf), d->rpm);
    nf_text(&nf_l, 780, 160, buf, C_TEXT, C_BG, 2);

    /* 底下兩條橫的踏板：上油門、下煞車 */
    hbar(24, 404, W - 48, 26, d->throttle, C_GREEN);
    hbar(24, 440, W - 48, 26, d->brake, C_RED);
}

/* 圈速：跑計時賽、練習時看。目前圈速放最大，加上跟最佳圈的差距。 */
static void page_timing(const dsh1_t *d, uint32_t now)
{
    char     buf[24];
    uint16_t c;

    gfx_clear(C_BG);
    draw_shift_lights(d, now);

    label(24, 72, "目前");
    fmt_lap(buf, sizeof(buf), d->lap_ms[0]);
    nf_text(&nf_l, 400, 100, buf, (d->lap_invalid == 1) ? C_RED : C_TEXT, C_BG, 1);

    gfx_fill_rect(0, 222, W, 2, C_LINE);

    label(24, 236, "上一圈");
    fmt_lap(buf, sizeof(buf), d->lap_ms[1]);
    nf_text(&nf_m, 24, 266, buf, C_TEXT, C_BG, 0);

    label(420, 236, "最佳");
    fmt_lap(buf, sizeof(buf), d->lap_ms[2]);
    nf_text(&nf_m, 420, 266, buf, C_PURPLE, C_BG, 0);

    gfx_fill_rect(0, 336, W, 2, C_LINE);

    label(24, 350, "差距");
    c = fmt_delta(buf, sizeof(buf), d->lap_ms[1], d->lap_ms[2]);
    nf_text(&nf_m, 24, 382, buf, c, C_BG, 0);

    label(330, 350, "圈數");
    fmt_laps(buf, sizeof(buf), d);
    nf_text(&nf_m, 330, 382, buf, C_TEXT, C_BG, 0);

    label(560, 350, "名次");
    fmt_int(buf, sizeof(buf), d->position);
    nf_text(&nf_m, 560, 382, buf, C_YELLOW, C_BG, 0);
}

/* 輪胎油量：耐久賽、要管輪胎跟油的時候看。 */
static void page_tyres(const dsh1_t *d, uint32_t now)
{
    static const char *const names[4] = { "左前", "右前", "左後", "右後" };
    char buf[24];

    gfx_clear(C_BG);
    draw_shift_lights(d, now);

    for (int i = 0; i < 4; i++) {
        int      x = 24 + (i & 1) * 220, y = 72 + (i >> 1) * 200;
        uint16_t bg = tyre_color(d->tyre_x10[i]);
        uint16_t fg = (d->tyre_x10[i] == DSH1_NA) ? C_DIM : C_BG;

        gfx_fill_rect(x, y, 204, 188, bg);
        gfx_text(x + 12, y + 10, names[i], fg);
        if (d->tyre_x10[i] == DSH1_NA) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            snprintf(buf, sizeof(buf), "%ld", (long)(d->tyre_x10[i] / 10));
        }
        nf_text(&nf_l, x + 102, y + 60, buf, fg, bg, 1);
    }

    label(480, 72, "油量");
    fmt_int(buf, sizeof(buf), d->fuel_pct);
    if (d->fuel_pct != DSH1_NA) { strcat(buf, "%"); }
    nf_text(&nf_l, 780, 100, buf, C_TEXT, C_BG, 2);
    hbar(480, 216, 300, 18, d->fuel_pct,
         (d->fuel_pct != DSH1_NA && d->fuel_pct < 15) ? C_RED : C_ORANGE);

    label(480, 256, "可跑");
    fmt_fuel_laps(buf, sizeof(buf), d->fuel_laps_x10);
    nf_text(&nf_m, 740, 284, buf, C_TEXT, C_BG, 2);
    gfx_text(748, 304, "圈", C_DIM);

    vbar(480, 356, 140, 100, d->throttle, C_GREEN);
    vbar(640, 356, 140, 100, d->brake, C_RED);
}

static const char *const s_page_names[DASH_PAGES] = {
    "完整", "大字", "圈速", "輪胎油量",
    "Classic", "GT3", "Radar", "Retro", "Mono", "Pocket",
};

const char *dash_page_name(int page)
{
    return s_page_names[(page >= 0 && page < DASH_PAGES) ? page : 0];
}

void dash_draw(int page, const dsh1_t *d, uint32_t now)
{
    switch (page) {
    case 1:  page_big(d, now);    break;
    case 2:  page_timing(d, now); break;
    case 3:  page_tyres(d, now);  break;
    case 4:  theme_classic(d, now); break;
    case 5:  theme_gt3(d, now);     break;
    case 6:  theme_radar(d, now);   break;
    case 7:  theme_retro(d, now);   break;
    case 8:  theme_mono(d, now);    break;
    case 9:  theme_pocket(d, now);  break;
    default: page_full(d, now);   break;
    }
}

/* 切換時在畫面下方中央顯示「2/4 大字」，讓使用者知道換到哪一頁。 */
void dash_draw_page_badge(int page)
{
    char buf[32];
    int  w;

    snprintf(buf, sizeof(buf), "%d/%d  %s", page + 1, DASH_PAGES, dash_page_name(page));
    w = gfx_text_width(buf) + 48;
    gfx_pill(W / 2 - w / 2, H - 76, w, 56, C_BLUE);
    gfx_text_center(W / 2, H - 60, buf, C_TEXT);
}

/* 長按叫出的選單：兩欄、每欄五個，點一下直接換過去。 */
#define MENU_X0   30
#define MENU_Y0   74
#define MENU_W    360
#define MENU_H    66
#define MENU_GAPX 20
#define MENU_GAPY 12

void dash_draw_menu(int current)
{
    gfx_fill_rect(0, 0, W, H, RGB565(10, 12, 18));
    gfx_text_center(W / 2, 26, "選擇儀表（點一下套用）", C_TEXT);
    for (int i = 0; i < DASH_PAGES; i++) {
        int      x = MENU_X0 + (i / 5) * (MENU_W + MENU_GAPX);
        int      y = MENU_Y0 + (i % 5) * (MENU_H + MENU_GAPY);
        char     buf[32];
        uint16_t bg = (i == current) ? C_BLUE : C_PANEL;

        gfx_pill(x, y, MENU_W, MENU_H, bg);
        snprintf(buf, sizeof(buf), "%d  %s", i + 1, dash_page_name(i));
        gfx_text_center(x + MENU_W / 2, y + 20, buf, C_TEXT);
    }
}

int dash_menu_hit(int x, int y)
{
    for (int i = 0; i < DASH_PAGES; i++) {
        int bx = MENU_X0 + (i / 5) * (MENU_W + MENU_GAPX);
        int by = MENU_Y0 + (i % 5) * (MENU_H + MENU_GAPY);

        if (x >= bx && x < bx + MENU_W && y >= by && y < by + MENU_H) { return i; }
    }
    return -1;
}

void dash_draw_waiting(const dash_link_t *link, uint32_t now)
{
    char buf[64];
    int  dots = (int)((now / 400u) % 4u);

    gfx_clear(C_BG);
    nf_text(&nf_l, W / 2, 90, "--", C_DIM, C_BG, 1);
    gfx_text_center(W / 2, 210, "RACE DASH", C_TEXT);

    snprintf(buf, sizeof(buf), "等待遊戲資料%.*s", dots, "...");
    gfx_text_center(W / 2, 260, buf, C_YELLOW);

    /* 診斷：使用者看到哪一行，就知道卡在哪一步。 */
    if (link->rx_bytes == 0) {
        gfx_text_center(W / 2, 320, "還沒收到任何資料：請確認 SimHub 已開啟", C_DIM);
        gfx_text_center(W / 2, 352, "Custom Serial device: COM, 115200", C_DIM);
    } else if (link->lines_ok == 0) {
        gfx_text_center(W / 2, 320, "有收到資料但格式不對：請確認公式", C_DIM);
    } else {
        gfx_text_center(W / 2, 320, "連線中，遊戲目前沒有在跑", C_DIM);
    }
    snprintf(buf, sizeof(buf), "RX %lu  OK %lu  BAD %lu",
             (unsigned long)link->rx_bytes, (unsigned long)link->lines_ok,
             (unsigned long)link->lines_bad);
    gfx_text_center(W / 2, 430, buf, C_LINE);
}
