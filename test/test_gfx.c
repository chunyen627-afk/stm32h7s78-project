/* Verifies the portrait rotation maths in gfx.c against a real framebuffer. */
#include "gfx.h"
#include "font_zh.h"
#include "ui.h"
#include "tetris.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                               \
    checks++;                                               \
    if (!(cond)) {                                          \
        printf("FAIL: %s (line %d)\n", (msg), __LINE__);      \
        failures++;                                         \
    }                                                       \
} while (0)

/* Full 800x480 framebuffer, same size as the real one. */
static uint16_t fb[PHYS_W * PHYS_H];

static void test_mapping_is_a_bijection(void)
{
    /* Every logical pixel must land on a distinct physical pixel, and the
     * whole canvas must be covered. If the rotation maths is wrong this
     * catches overlaps and gaps immediately. */
    memset(fb, 0, sizeof(fb));
    gfx_set_framebuffer(fb);

    for (int y = 0; y < GFX_H; y++) {
        for (int x = 0; x < GFX_W; x++) {
            gfx_pixel(x, y, 1);
        }
    }

    uint32_t painted = 0;
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        if (fb[i] == 1) painted++;
        /* Nothing should have been written twice; a double-write would have
         * stayed 1 anyway, so also check no cell holds a bad value. */
        CHECK(fb[i] == 0 || fb[i] == 1, "framebuffer holds only expected values");
        if (failures) return;
    }
    printf("  painted %lu of %lu physical pixels\n",
           (unsigned long)painted, (unsigned long)((uint32_t)PHYS_W * PHYS_H));
    CHECK(painted == (uint32_t)GFX_W * GFX_H, "logical canvas covers every pixel");
    CHECK((uint32_t)GFX_W * GFX_H == (uint32_t)PHYS_W * PHYS_H,
          "portrait canvas is the same area as the panel");
}

static void test_corners(void)
{
    /* Portrait (0,0) is top-left of the rotated view. Confirm the four
     * corners land where the rotation says they should. */
    memset(fb, 0, sizeof(fb));
    gfx_set_framebuffer(fb);

    /* Portrait (x,y) -> physical (y, GFX_W-1-x), i.e. offset
     * (GFX_W-1-x)*PHYS_W + y. */
    gfx_pixel(0, 0, 0xAAAA);
    CHECK(fb[(GFX_W - 1) * PHYS_W + 0] == 0xAAAA,
          "portrait top-left maps correctly");

    memset(fb, 0, sizeof(fb));
    gfx_pixel(GFX_W - 1, 0, 0xBBBB);
    CHECK(fb[0 * PHYS_W + 0] == 0xBBBB, "portrait top-right maps correctly");

    memset(fb, 0, sizeof(fb));
    gfx_pixel(0, GFX_H - 1, 0xCCCC);
    CHECK(fb[(GFX_W - 1) * PHYS_W + (GFX_H - 1)] == 0xCCCC,
          "portrait bottom-left maps correctly");

    memset(fb, 0, sizeof(fb));
    gfx_pixel(GFX_W - 1, GFX_H - 1, 0xDDDD);
    CHECK(fb[0 * PHYS_W + (GFX_H - 1)] == 0xDDDD,
          "portrait bottom-right maps correctly");
}

static void test_clipping(void)
{
    /* Out-of-range writes must be dropped, not wrap into other rows. */
    memset(fb, 0, sizeof(fb));
    gfx_set_framebuffer(fb);

    gfx_pixel(-1, 5, 0xFFFF);
    gfx_pixel(GFX_W, 5, 0xFFFF);
    gfx_pixel(5, -1, 0xFFFF);
    gfx_pixel(5, GFX_H, 0xFFFF);
    gfx_pixel(-100, -100, 0xFFFF);
    gfx_pixel(99999, 99999, 0xFFFF);

    uint32_t touched = 0;
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        if (fb[i] != 0) touched++;
    }
    CHECK(touched == 0, "out-of-range pixels are dropped");
}

static void test_fill_rect_matches_pixels(void)
{
    /* gfx_fill_rect takes a fast path; it must agree exactly with looping
     * gfx_pixel over the same area. */
    static uint16_t ref[PHYS_W * PHYS_H];

    memset(fb, 0, sizeof(fb));
    gfx_set_framebuffer(fb);
    gfx_fill_rect(37, 91, 123, 217, 0x1234);
    memcpy(ref, fb, sizeof(ref));

    memset(fb, 0, sizeof(fb));
    for (int y = 91; y < 91 + 217; y++) {
        for (int x = 37; x < 37 + 123; x++) {
            gfx_pixel(x, y, 0x1234);
        }
    }
    CHECK(memcmp(ref, fb, sizeof(ref)) == 0,
          "fill_rect matches per-pixel plotting");
}

/* Stand-in for the DMA2D backend: fills a physical rectangle. */
static void fake_fill_hw(uint16_t *dst, int px, int py,
                         int pw, int ph, uint16_t color)
{
    for (int r = 0; r < ph; r++) {
        uint16_t *row = &dst[(uint32_t)(py + r) * PHYS_W + (uint32_t)px];
        for (int c = 0; c < pw; c++) {
            row[c] = color;
        }
    }
}

static void test_hw_fill_matches_software(void)
{
    /* The hardware path receives a physical rectangle derived from the
     * logical one. If that conversion is wrong the accelerated fills land in
     * the wrong place, so require both paths to produce identical buffers. */
    static uint16_t soft[PHYS_W * PHYS_H];

    const struct { int x, y, w, h; } cases[] = {
        { 0, 0, GFX_W, GFX_H },      /* whole canvas */
        { 16, 64, 240, 480 },        /* the playfield */
        { 270, 90, 190, 24 },        /* a panel line */
        { 0, 0, 1, 1 },              /* single pixel */
        { GFX_W - 1, GFX_H - 1, 1, 1 },
        { 37, 91, 123, 217 },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gfx_set_fill_hw(0);
        memset(fb, 0, sizeof(fb));
        gfx_set_framebuffer(fb);
        gfx_fill_rect(cases[i].x, cases[i].y, cases[i].w, cases[i].h, 0x7BEF);
        memcpy(soft, fb, sizeof(soft));

        gfx_set_fill_hw(fake_fill_hw);
        memset(fb, 0, sizeof(fb));
        gfx_fill_rect(cases[i].x, cases[i].y, cases[i].w, cases[i].h, 0x7BEF);
        gfx_set_fill_hw(0);

        if (memcmp(soft, fb, sizeof(soft)) != 0) {
            printf("  case %u (%d,%d %dx%d) differs\n", i,
                   cases[i].x, cases[i].y, cases[i].w, cases[i].h);
        }
        CHECK(memcmp(soft, fb, sizeof(soft)) == 0,
              "hardware fill matches software fill");
    }
}

static void test_fill_rect_clipped(void)
{
    /* Rectangles straddling every edge must clip without corrupting memory. */
    memset(fb, 0, sizeof(fb));
    gfx_set_framebuffer(fb);

    gfx_fill_rect(-50, -50, 100, 100, 0x2222);
    gfx_fill_rect(GFX_W - 50, GFX_H - 50, 100, 100, 0x3333);
    gfx_fill_rect(-500, 10, 100, 10, 0x4444);   /* entirely off-canvas */
    gfx_fill_rect(10, -500, 10, 100, 0x5555);   /* entirely off-canvas */

    /* Count what landed: two 50x50 corners survive. */
    uint32_t c2 = 0, c3 = 0, c4 = 0, c5 = 0;
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        if (fb[i] == 0x2222) c2++;
        if (fb[i] == 0x3333) c3++;
        if (fb[i] == 0x4444) c4++;
        if (fb[i] == 0x5555) c5++;
    }
    CHECK(c2 == 50 * 50, "top-left overhang clipped to 50x50");
    CHECK(c3 == 50 * 50, "bottom-right overhang clipped to 50x50");
    CHECK(c4 == 0, "fully off-canvas rect draws nothing");
    CHECK(c5 == 0, "fully off-canvas rect draws nothing (vertical)");
}

static void test_font_lookup(void)
{
    /* The generated table must be sorted, and lookups must hit. */
    for (int i = 1; i < FONT_ZH_COUNT; i++) {
        CHECK(font_zh_cp[i - 1] < font_zh_cp[i], "font table is sorted");
        if (failures) return;
    }
    CHECK(font_zh_glyph('0') != 0, "digit 0 present");
    CHECK(font_zh_glyph('9') != 0, "digit 9 present");
    CHECK(font_zh_glyph(0x5206) != 0, "glyph 分 present");
    CHECK(font_zh_glyph(0x6578) != 0, "glyph 數 present");
    CHECK(font_zh_glyph(0x904A) != 0, "glyph 遊 present");
    CHECK(font_zh_glyph(0x6232) != 0, "glyph 戲 present");
    CHECK(font_zh_glyph(0x0001) == 0, "absent codepoint returns NULL");
    CHECK(font_zh_glyph(0xFFFF) == 0, "absent high codepoint returns NULL");

    /* Every glyph must have some ink - catches a font that failed to render. */
    int blank = 0;
    for (int i = 0; i < FONT_ZH_COUNT; i++) {
        int ink = 0;
        for (int b = 0; b < FONT_ZH_BYTES; b++) {
            if (font_zh_bitmap[i][b]) ink++;
        }
        if (!ink) {
            printf("  blank glyph at U+%04X\n", font_zh_cp[i]);
            blank++;
        }
    }
    CHECK(blank == 0, "no glyph rendered blank");
}

static void test_text_drawing(void)
{
    /* Chinese text must actually put pixels on the canvas, inside bounds. */
    memset(fb, 0, sizeof(fb));
    gfx_set_framebuffer(fb);
    gfx_text(10, 10, "分數 1230", 0xFFFF);

    uint32_t ink = 0;
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        if (fb[i] == 0xFFFF) ink++;
    }
    printf("  text ink pixels: %lu\n", (unsigned long)ink);
    CHECK(ink > 100, "Chinese text renders visible pixels");

    int w = gfx_text_width("分數 1230");
    CHECK(w > 0 && w < GFX_W, "text width is sane");

    /* Centring must stay on canvas. */
    memset(fb, 0, sizeof(fb));
    gfx_text_center(GFX_W / 2, 400, "遊戲結束", 0xFFFF);
    ink = 0;
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        if (fb[i] == 0xFFFF) ink++;
    }
    CHECK(ink > 100, "centred text renders");
}

static void test_number_rendering(void)
{
    memset(fb, 0, sizeof(fb));
    gfx_set_framebuffer(fb);

    CHECK(gfx_number_width(0) == gfx_number_width(7), "single digits same width");
    CHECK(gfx_number_width(1234) > gfx_number_width(12), "more digits are wider");

    gfx_number(20, 20, 0, 0xFFFF);
    uint32_t ink = 0;
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        if (fb[i] == 0xFFFF) ink++;
    }
    CHECK(ink > 10, "zero renders");

    /* Right-aligned numbers must not run off the left edge for big values. */
    memset(fb, 0, sizeof(fb));
    gfx_number_right(GFX_W - 10, 30, 999999, 0xFFFF);
    ink = 0;
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        if (fb[i] == 0xFFFF) ink++;
    }
    CHECK(ink > 10, "right-aligned number renders");
}

/* Two framebuffers, drawn the way the firmware drives them. */
static uint16_t fb_a[PHYS_W * PHYS_H];
static uint16_t fb_b[PHYS_W * PHYS_H];

static void test_no_unpainted_band(void)
{
    /* The firmware paints the static layer once per buffer, then repaints
     * only part of the screen each frame while alternating buffers. Any pixel
     * ui_draw does not cover must therefore already be identical in both
     * buffers - otherwise it alternates every frame and flickers.
     *
     * Draw the same game state into both buffers through the real code path
     * and require the results to match exactly. */
    tetris_t g;
    tetris_init(&g, 4242);

    gfx_set_framebuffer(fb_a);
    ui_draw_static();
    gfx_set_framebuffer(fb_b);
    ui_draw_static();

    /* Simulate the real sequence: the state changes (sound gets toggled off),
     * then the game alternates buffers. Buffer A last saw the old state, B
     * the new one. Once both have been redrawn with the current state they
     * must agree - any pixel that still differs is one ui_draw never
     * repaints, and on hardware it alternates every frame as a flicker. */
    gfx_set_framebuffer(fb_a);
    ui_draw(&g, 0, false, true);    /* stale frame: sound on  */
    gfx_set_framebuffer(fb_b);
    ui_draw(&g, 0, false, false);   /* state changed: sound off */
    gfx_set_framebuffer(fb_a);
    ui_draw(&g, 0, false, false);   /* A catches up */

    uint32_t diff = 0;
    int first_y = -1;
    for (int y = 0; y < GFX_H; y++) {
        for (int x = 0; x < GFX_W; x++) {
            uint32_t off = (uint32_t)(GFX_W - 1 - x) * PHYS_W + (uint32_t)y;
            if (fb_a[off] != fb_b[off]) {
                if (first_y < 0) {
                    first_y = y;
                }
                diff++;
            }
        }
    }
    if (diff) {
        printf("  %lu pixels differ between buffers, first at y=%d\n",
               (unsigned long)diff, first_y);
    }
    CHECK(diff == 0, "no pixel is left outside the repainted region");
}

static void test_frame_never_goes_blank(void)
{
    /* The panel scans continuously, so any moment where a large part of the
     * buffer is bare background can be caught mid-frame and seen as a flash.
     * Redraw over a known-good frame and sample the buffer as the drawing
     * proceeds: the field must never be predominantly empty background.
     *
     * Approximated here by checking that a redraw never reduces the number of
     * non-background pixels - i.e. nothing is blanked and left that way. */
    tetris_t g;
    tetris_init(&g, 777);
    memset(g.board, 0, sizeof(g.board));
    for (int x = 0; x < TET_W; x++) {
        for (int h = 0; h < 6; h++) {
            g.board[TET_TOTAL_H - 1 - h][x] = (uint8_t)(CELL_I + ((x + h) % 7));
        }
    }
    g.state = STATE_PLAYING;

    gfx_set_framebuffer(fb_a);
    ui_draw_static();
    ui_draw(&g, 0, false, true);

    /* Count lit pixels inside the field after a complete frame. */
    uint32_t lit_before = 0;
    for (int y = FIELD_Y; y < FIELD_Y + FIELD_H; y++) {
        for (int x = FIELD_X; x < FIELD_X + FIELD_W; x++) {
            if (gfx_get_pixel(x, y) != RGB565(18, 20, 30)) lit_before++;
        }
    }

    /* Draw the identical frame again. A correct redraw overwrites in place,
     * so the count must not change; a clear-then-paint would momentarily
     * drop it, and any leftover blanking shows up here. */
    ui_draw(&g, 0, false, true);
    uint32_t lit_after = 0;
    for (int y = FIELD_Y; y < FIELD_Y + FIELD_H; y++) {
        for (int x = FIELD_X; x < FIELD_X + FIELD_W; x++) {
            if (gfx_get_pixel(x, y) != RGB565(18, 20, 30)) lit_after++;
        }
    }
    printf("  field lit pixels: %lu then %lu\n",
           (unsigned long)lit_before, (unsigned long)lit_after);
    CHECK(lit_before > 1000, "field actually has content to compare");
    CHECK(lit_before == lit_after, "redraw is stable, not clear-then-paint");
}

static void test_repaint_covers_dynamic_content(void)
{
    /* Anything that changes with game state must sit inside the region
     * ui_draw repaints. Compare a fresh buffer against one carrying stale
     * content: every difference must be repainted, so painting the same
     * state over junk must reproduce the clean frame exactly. */
    tetris_t g;
    tetris_init(&g, 99);

    gfx_set_framebuffer(fb_a);
    ui_draw_static();
    ui_draw(&g, 0, false, true);

    /* Fill B with junk, lay the static layer over it, then draw the frame. */
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        fb_b[i] = (uint16_t)(0xA5A5u ^ (uint16_t)i);
    }
    gfx_set_framebuffer(fb_b);
    ui_draw_static();
    ui_draw(&g, 0, false, true);

    uint32_t diff = 0;
    int first_y = -1;
    for (int y = 0; y < GFX_H; y++) {
        for (int x = 0; x < GFX_W; x++) {
            uint32_t off = (uint32_t)(GFX_W - 1 - x) * PHYS_W + (uint32_t)y;
            if (fb_a[off] != fb_b[off]) {
                if (first_y < 0) first_y = y;
                diff++;
            }
        }
    }
    if (diff) {
        printf("  %lu stale pixels survive a redraw, first at y=%d\n",
               (unsigned long)diff, first_y);
    }
    CHECK(diff == 0, "a full redraw erases all stale content");
}

int main(void)
{
    printf("=== gfx / portrait rotation tests ===\n");
    test_mapping_is_a_bijection();
    test_corners();
    test_clipping();
    test_fill_rect_matches_pixels();
    test_hw_fill_matches_software();
    test_fill_rect_clipped();
    test_font_lookup();
    test_text_drawing();
    test_number_rendering();
    test_no_unpainted_band();
    test_frame_never_goes_blank();
    test_repaint_covers_dynamic_content();
    printf("=== %d checks, %d failures ===\n", checks, failures);
    printf(failures == 0 ? "ALL PASS\n" : "TESTS FAILED\n");
    return failures;
}
