#include "gfx.h"
#include "font_zh.h"

static uint16_t *g_fb;
static gfx_fill_hw_fn g_fill_hw;
static gfx_sync_hw_fn g_sync_hw;

void gfx_set_framebuffer(uint16_t *fb) { g_fb = fb; }
uint16_t *gfx_framebuffer(void)        { return g_fb; }
void gfx_set_fill_hw(gfx_fill_hw_fn fn) { g_fill_hw = fn; }
void gfx_set_sync_hw(gfx_sync_hw_fn fn) { g_sync_hw = fn; }

/* 在 CPU 動到 framebuffer 之前呼叫，確保非同步的硬體填色已經結束。 */
static inline void sync_hw(void)
{
    if (g_sync_hw) {
        g_sync_hw();
    }
}

/* Portrait (x,y) -> physical linear offset.
 * The panel is landscape and the portrait canvas is rotated 90 degrees
 * anticlockwise onto it: physical_x = y, physical_y = GFX_W-1-x. */
static inline uint32_t map_offset(int x, int y)
{
    return (uint32_t)(GFX_W - 1 - x) * PHYS_W + (uint32_t)y;
}

/* 注意：這裡刻意不做硬體同步。逐像素檢查在畫文字時會呼叫上千次，成本太高。
 * 同步交給呼叫端在迴圈外做一次（gfx_char、gfx_circle 等都已處理）。 */
void gfx_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H || !g_fb) {
        return;
    }
    g_fb[map_offset(x, y)] = color;
}

uint16_t gfx_get_pixel(int x, int y)
{
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H || !g_fb) {
        return 0;
    }
    sync_hw();
    return g_fb[map_offset(x, y)];
}

void gfx_clear(uint16_t color)
{
    if (!g_fb) {
        return;
    }
    sync_hw();
    uint32_t n = (uint32_t)PHYS_W * PHYS_H;
    for (uint32_t i = 0; i < n; i++) {
        g_fb[i] = color;
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!g_fb || w <= 0 || h <= 0) {
        return;
    }
    /* Clip to the logical canvas. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) { w = GFX_W - x; }
    if (y + h > GFX_H) { h = GFX_H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    /* A portrait rectangle is still a rectangle once rotated: logical x maps
     * to physical rows and logical y to physical columns. */
    if (g_fill_hw) {
        /* Physical origin is the logical rect's top-right corner. */
        g_fill_hw(g_fb, y, GFX_W - (x + w), h, w, color);
        return;
    }

    /* A portrait column is a physical row, so for each logical x walk the
     * y range as a contiguous physical run. */
    sync_hw();
    for (int xx = x; xx < x + w; xx++) {
        uint16_t *p = &g_fb[(uint32_t)(GFX_W - 1 - xx) * PHYS_W + (uint32_t)y];
        for (int yy = 0; yy < h; yy++) {
            p[yy] = color;
        }
    }
}

void gfx_rect(int x, int y, int w, int h, uint16_t color)
{
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

/* Scale a colour channel-wise; f is 0..255 where 128 is unchanged. */
static uint16_t shade(uint16_t c, int f)
{
    int r = ((c >> 11) & 0x1F) * f / 128;
    int g = ((c >> 5)  & 0x3F) * f / 128;
    int b = (c         & 0x1F) * f / 128;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void gfx_block(int x, int y, int w, int h, uint16_t color)
{
    gfx_fill_rect(x, y, w, h, color);
    /* Highlight top and left, shadow bottom and right. */
    uint16_t hi = shade(color, 170);
    uint16_t lo = shade(color, 85);
    gfx_fill_rect(x, y, w, 2, hi);
    gfx_fill_rect(x, y, 2, h, hi);
    gfx_fill_rect(x, y + h - 2, w, 2, lo);
    gfx_fill_rect(x + w - 2, y, 2, h, lo);
}

void gfx_disc(int cx, int cy, int r, uint16_t color)
{
    if (r <= 0) {
        return;
    }
    /* Scan each row and fill the chord that lies inside the circle. */
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        /* Widest dx with dx*dx + dy*dy <= r*r, found without a square root. */
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) {
            dx++;
        }
        gfx_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

void gfx_circle(int cx, int cy, int r, uint16_t color)
{
    if (r <= 0) {
        return;
    }
    /* Midpoint circle, plotting all eight octants. */
    sync_hw();
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, color);
        gfx_pixel(cx + y, cy + x, color);
        gfx_pixel(cx - y, cy + x, color);
        gfx_pixel(cx - x, cy + y, color);
        gfx_pixel(cx - x, cy - y, color);
        gfx_pixel(cx - y, cy - x, color);
        gfx_pixel(cx + y, cy - x, color);
        gfx_pixel(cx + x, cy - y, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_pill(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int r = h / 2;
    /* Middle bar plus a disc on each end. */
    gfx_fill_rect(x + r, y, w - 2 * r, h, color);
    gfx_disc(x + r, y + r, r, color);
    gfx_disc(x + w - r - 1, y + r, r, color);
}

void gfx_char(int x, int y, uint16_t cp, uint16_t color)
{
    const uint8_t *glyph = font_zh_glyph(cp);
    if (!glyph) {
        return;
    }
    /* 一個字約寫幾百個像素，在這裡同步一次即可。 */
    sync_hw();
    for (int r = 0; r < FONT_ZH_SIZE; r++) {
        const uint8_t *row = &glyph[r * FONT_ZH_STRIDE];
        for (int c = 0; c < FONT_ZH_SIZE; c++) {
            if ((row[c >> 3] >> (7 - (c & 7))) & 1) {
                gfx_pixel(x + c, y + r, color);
            }
        }
    }
}

/* Decode one UTF-8 codepoint; advances *p. Returns 0 at end of string. */
static uint16_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!*s) {
        return 0;
    }
    uint16_t cp;
    if (*s < 0x80) {
        cp = *s;
        s += 1;
    } else if ((*s & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = (uint16_t)(((*s & 0x1F) << 6) | (s[1] & 0x3F));
        s += 2;
    } else if ((*s & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80) {
        cp = (uint16_t)(((*s & 0x0F) << 12) | ((s[1] & 0x3F) << 6) |
                        (s[2] & 0x3F));
        s += 3;
    } else {
        /* Unsupported sequence: skip one byte so we always make progress. */
        cp = '?';
        s += 1;
    }
    *p = (const char *)s;
    return cp;
}

/* Latin glyphs look better tightened up; Chinese stays full width. */
static int advance_for(uint16_t cp)
{
    return (cp < 0x80) ? (FONT_ZH_SIZE / 2 + 2) : (FONT_ZH_SIZE + 1);
}

void gfx_text(int x, int y, const char *utf8, uint16_t color)
{
    const char *p = utf8;
    uint16_t cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp == ' ') {
            x += FONT_ZH_SIZE / 2;
            continue;
        }
        /* Latin glyphs are rendered in a full cell but advanced narrowly, so
         * nudge them left to keep the spacing even. */
        int nudge = (cp < 0x80) ? -(FONT_ZH_SIZE / 4) : 0;
        gfx_char(x + nudge, y, cp, color);
        x += advance_for(cp);
    }
}

int gfx_text_width(const char *utf8)
{
    const char *p = utf8;
    uint16_t cp;
    int w = 0;
    while ((cp = utf8_next(&p)) != 0) {
        w += (cp == ' ') ? (FONT_ZH_SIZE / 2) : advance_for(cp);
    }
    return w;
}

void gfx_text_center(int cx, int y, const char *utf8, uint16_t color)
{
    gfx_text(cx - gfx_text_width(utf8) / 2, y, utf8, color);
}

int gfx_number_width(uint32_t value)
{
    int digits = 1;
    for (uint32_t v = value; v >= 10; v /= 10) {
        digits++;
    }
    return digits * advance_for('0');
}

void gfx_number(int x, int y, uint32_t value, uint16_t color)
{
    char buf[12];
    int n = 0;
    if (value == 0) {
        buf[n++] = '0';
    }
    while (value > 0 && n < (int)sizeof(buf) - 1) {
        buf[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    /* Digits came out reversed. */
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i];
        buf[i] = buf[n - 1 - i];
        buf[n - 1 - i] = t;
    }
    buf[n] = '\0';
    gfx_text(x, y, buf, color);
}

void gfx_number_right(int right_x, int y, uint32_t value, uint16_t color)
{
    gfx_number(right_x - gfx_number_width(value), y, value, color);
}
