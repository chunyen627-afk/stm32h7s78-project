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

/* 兩種方向裡較長的那一邊。要開「一列夠長」的緩衝區時用這個，
 * 照 GFX_W 開的話橫向模式下寬度變 800 會直接越界。 */
#define GFX_MAX_DIM  800

/* 畫布方向。
 *
 * 面板實體是 800x480 橫向，預設把邏輯 480x800 直立畫布旋轉映射上去。
 * 切成橫向時直接 1:1 對應，畫布變成 800x480 —— 橫式照片因此能顯示到
 * 720x480 而不是 480x320，面積是 2.25 倍。
 *
 * 預設直立，所以沒呼叫這個函式的專案（例如 tetris）行為完全不變。
 * GFX_W / GFX_H 仍然是「直立」的尺寸，選單那類固定版面照舊用它們；
 * 需要跟著方向走的程式碼改用 gfx_width() / gfx_height()。 */
void gfx_set_orientation(bool landscape);
bool gfx_is_landscape(void);
int  gfx_width(void);
int  gfx_height(void);

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

/* 讓繪圖層在 CPU 直接寫入 framebuffer 之前，先等硬體停下來。
 *
 * 硬體填色是非同步的，發出後函式就返回。任何隨後的 CPU 寫入若沒等它結束，
 * 會被硬體正在進行的傳輸蓋掉。文字、線條、圓形都是 CPU 逐像素畫的，所以
 * 這個同步點必須在繪圖層裡，不能只靠呼叫端記得。 */
typedef void (*gfx_sync_hw_fn)(void);
void gfx_set_sync_hw(gfx_sync_hw_fn fn);

void gfx_clear(uint16_t color);
void gfx_pixel(int x, int y, uint16_t color);
/* Read a pixel back; returns 0 outside the canvas.
 *
 * 注意：當硬體填色啟用時，DMA2D 是繞過 D-Cache 直接寫 PSRAM 的，CPU 從這裡
 * 讀到的可能是快取中的舊值。若真的需要讀回畫過的內容，讀之前要先呼叫
 * SCB_InvalidateDCache_by_Addr()。目前遊戲不依賴讀回，所以沒有在這裡做
 * （每次呼叫都失效快取會很慢）。 */
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
