/**
 * Portrait drawing layer.
 *
 * The panel is physically 800x480 landscape and the BSP only supports
 * LCD_ORIENTATION_LANDSCAPE, so this layer presents a 480x800 portrait
 * canvas and rotates every coordinate on the way to the framebuffer.
 *
 * Portrait (x, y) maps to physical (PHYS_W - 1 - y, x).
 */
#ifndef GFX_H_INCLUDED
#define GFX_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#define PHYS_W  800
#define PHYS_H  480

/* Logical portrait canvas. */
#define GFX_W   480
#define GFX_H   800

/* RGB565 helpers. */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void gfx_set_framebuffer(uint16_t *fb);
uint16_t *gfx_framebuffer(void);

/* Optional hardware-accelerated rectangle fill. The firmware installs a
 * DMA2D-backed implementation; when none is set, fills run on the CPU.
 * Coordinates are physical (landscape) framebuffer coordinates. */
typedef void (*gfx_fill_hw_fn)(uint16_t *dst, int px, int py,
                               int pw, int ph, uint16_t color);
void gfx_set_fill_hw(gfx_fill_hw_fn fn);

void gfx_clear(uint16_t color);
void gfx_pixel(int x, int y, uint16_t color);
/* Read a pixel back; returns 0 outside the canvas. */
uint16_t gfx_get_pixel(int x, int y);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void gfx_rect(int x, int y, int w, int h, uint16_t color);
/* Rounded-ish block used for tetromino cells: fill plus a lighter top-left
 * edge and darker bottom-right edge, so the stack reads as 3D. */
void gfx_block(int x, int y, int w, int h, uint16_t color);
/* Filled circle centred at (cx, cy). */
void gfx_disc(int cx, int cy, int r, uint16_t color);
/* Circle outline. */
void gfx_circle(int cx, int cy, int r, uint16_t color);
/* Rounded rectangle, used for pill-shaped buttons. */
void gfx_pill(int x, int y, int w, int h, uint16_t color);

/* Text. Strings are UTF-8; Chinese glyphs come from font_zh. */
void gfx_char(int x, int y, uint16_t cp, uint16_t color);
void gfx_text(int x, int y, const char *utf8, uint16_t color);
/* Width in pixels the string would occupy. */
int  gfx_text_width(const char *utf8);
void gfx_text_center(int cx, int y, const char *utf8, uint16_t color);

/* Unsigned integer rendered with the bitmap digits. */
void gfx_number(int x, int y, uint32_t value, uint16_t color);
int  gfx_number_width(uint32_t value);
void gfx_number_right(int right_x, int y, uint32_t value, uint16_t color);

#endif /* GFX_H_INCLUDED */
