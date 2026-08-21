#include "ui.h"
#include "font_zh.h"

/* Tetromino colours, indexed by cell_t. */
static const uint16_t CELL_COLOR[CELL_COUNT] = {
    [CELL_EMPTY] = RGB565(18, 20, 30),
    [CELL_I]     = RGB565(0, 200, 220),
    [CELL_J]     = RGB565(40, 90, 220),
    [CELL_L]     = RGB565(240, 150, 30),
    [CELL_O]     = RGB565(235, 210, 40),
    [CELL_S]     = RGB565(60, 210, 90),
    [CELL_T]     = RGB565(190, 80, 220),
    [CELL_Z]     = RGB565(230, 60, 70),
};

#define COL_BG        RGB565(10, 12, 20)
#define COL_GRID      RGB565(30, 34, 48)
#define COL_FRAME     RGB565(70, 80, 110)
#define COL_TEXT      RGB565(235, 238, 245)
#define COL_DIM       RGB565(130, 140, 160)
#define COL_GHOST     RGB565(60, 66, 84)
#define COL_FLASH     RGB565(255, 255, 255)

/* Control surfaces: dark grey plastic, like a handheld's shell. */
#define COL_PAD       RGB565(52, 56, 70)
#define COL_PAD_EDGE  RGB565(88, 94, 115)
#define COL_HELD      RGB565(120, 130, 160)

/* SNES face-button colours. */
#define COL_A         RGB565(120, 90, 200)   /* A - purple */
#define COL_B         RGB565(210, 170, 40)   /* B - yellow */
#define COL_X         RGB565(60, 110, 200)   /* X - blue   */
#define COL_Y         RGB565(60, 170, 90)    /* Y - green  */

/* D-pad arms are rectangles arranged in a cross; face buttons are a diamond.
 * Bounding boxes here are what ui_hit_test uses, so they must match what is
 * drawn closely enough that presses land where they look.
 *
 * The arms stop short of the hub so their boxes do not overlap - otherwise
 * the centre of the pad would always resolve to whichever arm is checked
 * first, and pressing the middle would move the piece sideways. */
const btn_rect_t UI_BUTTONS[BTN_COUNT] = {
    [BTN_LEFT]  = { DPAD_CX - DPAD_ARM - DPAD_HALF, DPAD_CY - DPAD_HALF,
                    DPAD_ARM, 2 * DPAD_HALF, SHAPE_RECT, 0, "" },
    [BTN_RIGHT] = { DPAD_CX + DPAD_HALF, DPAD_CY - DPAD_HALF,
                    DPAD_ARM, 2 * DPAD_HALF, SHAPE_RECT, 0, "" },
    [BTN_HARD]  = { DPAD_CX - DPAD_HALF, DPAD_CY - DPAD_ARM - DPAD_HALF,
                    2 * DPAD_HALF, DPAD_ARM, SHAPE_RECT, 0, "" },
    [BTN_SOFT]  = { DPAD_CX - DPAD_HALF, DPAD_CY + DPAD_HALF,
                    2 * DPAD_HALF, DPAD_ARM, SHAPE_RECT, 0, "" },

    /* Diamond: A right, B bottom, X top, Y left. */
    [BTN_ROT_CW]  = { FACE_CX + FACE_GAP - FACE_R, FACE_CY - FACE_R,
                      2 * FACE_R, 2 * FACE_R, SHAPE_CIRCLE, COL_A, "A" },
    [BTN_ROT_CCW] = { FACE_CX - FACE_R, FACE_CY + FACE_GAP - FACE_R,
                      2 * FACE_R, 2 * FACE_R, SHAPE_CIRCLE, COL_B, "B" },
    [BTN_SOUND]   = { FACE_CX - FACE_R, FACE_CY - FACE_GAP - FACE_R,
                      2 * FACE_R, 2 * FACE_R, SHAPE_CIRCLE, COL_X, "X" },
    [BTN_HOLD]    = { FACE_CX - FACE_GAP - FACE_R, FACE_CY - FACE_R,
                      2 * FACE_R, 2 * FACE_R, SHAPE_CIRCLE, COL_Y, "Y" },

    /* Pause centred along the bottom edge, restart to its right. */
    [BTN_PAUSE]   = { GFX_W / 2 - SYS_W / 2, SYS_Y, SYS_W, SYS_H,
                      SHAPE_PILL, 0, "暫停" },
    [BTN_RESTART] = { GFX_W - 16 - SYS_W, SYS_Y, SYS_W, SYS_H,
                      SHAPE_PILL, 0, "重來" },
};

btn_id_t ui_hit_test(int x, int y)
{
    /* Circles need a radial test so the gaps between the face buttons do not
     * register as presses. */
    for (int i = 0; i < BTN_COUNT; i++) {
        const btn_rect_t *b = &UI_BUTTONS[i];
        if (x < b->x || x >= b->x + b->w || y < b->y || y >= b->y + b->h) {
            continue;
        }
        if (b->shape == SHAPE_CIRCLE) {
            int cx = b->x + b->w / 2;
            int cy = b->y + b->h / 2;
            int r  = b->w / 2;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
        }
        return (btn_id_t)i;
    }
    return BTN_COUNT;
}

static void draw_cell(int cx, int cy, uint16_t color)
{
    gfx_block(FIELD_X + cx * CELL, FIELD_Y + cy * CELL, CELL, CELL, color);
}

/* Paint one empty cell, including the grid lines along its edges, so an empty
 * square can be restored without clearing the whole field first. */
static void draw_empty_cell(int cx, int cy)
{
    int px = FIELD_X + cx * CELL;
    int py = FIELD_Y + cy * CELL;
    gfx_fill_rect(px, py, CELL, CELL, CELL_COLOR[CELL_EMPTY]);
    if (cx > 0) {
        gfx_fill_rect(px, py, 1, CELL, COL_GRID);
    }
    if (cy > 0) {
        gfx_fill_rect(px, py, CELL, 1, COL_GRID);
    }
}

static void draw_field(const tetris_t *g)
{
    gfx_rect(FIELD_X - 2, FIELD_Y - 2, FIELD_W + 4, FIELD_H + 4, COL_FRAME);

    /* Every cell is written every frame - either its block colour or the
     * empty background - so the field never needs a separate clear pass and
     * is never momentarily blank. */
    int ghost_y = (g->state == STATE_PLAYING) ? tetris_ghost_y(g) : -1000;

    for (int y = 0; y < TET_H; y++) {
        bool flash = tetris_row_clearing(g, y);
        for (int x = 0; x < TET_W; x++) {
            uint8_t v = tetris_cell_at(g, x, y);
            if (flash) {
                draw_cell(x, y, COL_FLASH);
            } else if (v != CELL_EMPTY) {
                draw_cell(x, y, CELL_COLOR[v]);
            } else {
                draw_empty_cell(x, y);
            }
        }
    }

    /* Ghost outline goes on top of the empty cells it sits in. */
    if (g->state == STATE_PLAYING && ghost_y != g->cur.y) {
        for (int cy = 0; cy < 4; cy++) {
            for (int cx = 0; cx < 4; cx++) {
                if (!tetris_shape_cell(g->cur.kind, g->cur.rot, cx, cy)) {
                    continue;
                }
                int bx = g->cur.x + cx;
                int by = ghost_y + cy - TET_HIDDEN;
                if (bx < 0 || bx >= TET_W || by < 0 || by >= TET_H) {
                    continue;
                }
                if (tetris_cell_at(g, bx, by) != CELL_EMPTY) {
                    continue;
                }
                gfx_rect(FIELD_X + bx * CELL + 2, FIELD_Y + by * CELL + 2,
                         CELL - 4, CELL - 4, COL_GHOST);
            }
        }
    }
}

static void draw_preview(int bx, int by, int bw, int bh, piece_t kind)
{
    /* Fill the box so the previous piece does not show through; the box is
     * small enough that repainting it is never visible as a flash. */
    gfx_fill_rect(bx, by, bw, bh, COL_BG);
    gfx_rect(bx, by, bw, bh, COL_FRAME);
    if (kind >= PIECE_COUNT) {
        return;
    }
    const int pc = 14;

    int minx = 4, maxx = -1, miny = 4, maxy = -1;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (tetris_preview_cell(kind, x, y)) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx < minx) {
        return;
    }
    int pw = (maxx - minx + 1) * pc;
    int ph = (maxy - miny + 1) * pc;
    int ox = bx + (bw - pw) / 2 - minx * pc;
    int oy = by + (bh - ph) / 2 - miny * pc;

    static const cell_t PC_CELL[PIECE_COUNT] = {
        CELL_I, CELL_J, CELL_L, CELL_O, CELL_S, CELL_T, CELL_Z
    };
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (tetris_preview_cell(kind, x, y)) {
                gfx_block(ox + x * pc, oy + y * pc, pc, pc,
                          CELL_COLOR[PC_CELL[kind]]);
            }
        }
    }
}

/* Numbers shrink as well as grow, so the line is wiped before drawing. With
 * the buffer swap fixed this is invisible - it happens in the back buffer. */
static void draw_number_line(int x, int y, uint32_t value)
{
    gfx_fill_rect(x, y, PANEL_W - 6, FONT_ZH_SIZE, COL_BG);
    gfx_number(x, y, value, COL_TEXT);
}

void ui_invalidate(void)
{
    /* Nothing is cached any more; kept so callers need not change. */
}

static void draw_panel(const tetris_t *g)
{
    piece_t hold = g->has_hold ? g->hold : (piece_t)PIECE_COUNT;

    int x = PANEL_X;
    int y = FIELD_Y;

    gfx_text(x, y, "分數", COL_DIM);        y += 26;
    draw_number_line(x, y, g->score);       y += 36;

    gfx_text(x, y, "等級", COL_DIM);        y += 26;
    draw_number_line(x, y, g->level);       y += 36;

    gfx_text(x, y, "行數", COL_DIM);        y += 26;
    draw_number_line(x, y, g->lines);       y += 40;

    gfx_text(x, y, "下一個", COL_DIM);      y += 26;
    draw_preview(x, y, PANEL_W - 6, 68, g->next);
    y += 80;

    gfx_text(x, y, "保留", COL_DIM);        y += 26;
    draw_preview(x, y, PANEL_W - 6, 68, hold);
}

/* Small solid triangle pointing in the given direction, apex at the far end.
 * dir: 0=left 1=right 2=up 3=down. */
static void draw_arrow(int cx, int cy, int dir, uint16_t color)
{
    const int s = 9;
    /* i counts outward from the base to the apex, so the row length shrinks. */
    for (int i = 0; i < s; i++) {
        int len = 2 * (s - i) - 1;
        switch (dir) {
            case 0:  /* apex to the left: base on the right, narrowing leftward */
                gfx_fill_rect(cx - i, cy - len / 2, 1, len, color);
                break;
            case 1:  /* apex to the right */
                gfx_fill_rect(cx + i, cy - len / 2, 1, len, color);
                break;
            case 2:  /* apex upward */
                gfx_fill_rect(cx - len / 2, cy - i, len, 1, color);
                break;
            default: /* apex downward */
                gfx_fill_rect(cx - len / 2, cy + i, len, 1, color);
                break;
        }
    }
}

static void draw_dpad(uint32_t held)
{
    bool l = (held >> BTN_LEFT)  & 1u;
    bool r = (held >> BTN_RIGHT) & 1u;
    bool u = (held >> BTN_HARD)  & 1u;
    bool d = (held >> BTN_SOFT)  & 1u;

    /* Fill both bars first with no outlines. Outlining each bar separately
     * would draw the vertical bar's edge straight across the middle of the
     * horizontal one, chopping the cross into disconnected pieces. */
    gfx_fill_rect(DPAD_CX - DPAD_ARM - DPAD_HALF, DPAD_CY - DPAD_HALF,
                  2 * (DPAD_ARM + DPAD_HALF), 2 * DPAD_HALF, COL_PAD);
    gfx_fill_rect(DPAD_CX - DPAD_HALF, DPAD_CY - DPAD_ARM - DPAD_HALF,
                  2 * DPAD_HALF, 2 * (DPAD_ARM + DPAD_HALF), COL_PAD);

    /* Outline the cross as one shape: each bar's long edges, stopping where
     * the other bar joins. */
    gfx_fill_rect(DPAD_CX - DPAD_ARM - DPAD_HALF, DPAD_CY - DPAD_HALF,
                  2 * (DPAD_ARM + DPAD_HALF), 1, COL_PAD_EDGE);
    gfx_fill_rect(DPAD_CX - DPAD_ARM - DPAD_HALF, DPAD_CY + DPAD_HALF - 1,
                  2 * (DPAD_ARM + DPAD_HALF), 1, COL_PAD_EDGE);
    gfx_fill_rect(DPAD_CX - DPAD_HALF, DPAD_CY - DPAD_ARM - DPAD_HALF,
                  1, 2 * (DPAD_ARM + DPAD_HALF), COL_PAD_EDGE);
    gfx_fill_rect(DPAD_CX + DPAD_HALF - 1, DPAD_CY - DPAD_ARM - DPAD_HALF,
                  1, 2 * (DPAD_ARM + DPAD_HALF), COL_PAD_EDGE);
    /* Caps at the four tips. */
    gfx_fill_rect(DPAD_CX - DPAD_ARM - DPAD_HALF, DPAD_CY - DPAD_HALF,
                  1, 2 * DPAD_HALF, COL_PAD_EDGE);
    gfx_fill_rect(DPAD_CX + DPAD_ARM + DPAD_HALF - 1, DPAD_CY - DPAD_HALF,
                  1, 2 * DPAD_HALF, COL_PAD_EDGE);
    gfx_fill_rect(DPAD_CX - DPAD_HALF, DPAD_CY - DPAD_ARM - DPAD_HALF,
                  2 * DPAD_HALF, 1, COL_PAD_EDGE);
    gfx_fill_rect(DPAD_CX - DPAD_HALF, DPAD_CY + DPAD_ARM + DPAD_HALF - 1,
                  2 * DPAD_HALF, 1, COL_PAD_EDGE);

    if (l) {
        gfx_fill_rect(DPAD_CX - DPAD_ARM - DPAD_HALF + 1, DPAD_CY - DPAD_HALF + 1,
                      DPAD_ARM - 1, 2 * DPAD_HALF - 2, COL_HELD);
    }
    if (r) {
        gfx_fill_rect(DPAD_CX + DPAD_HALF, DPAD_CY - DPAD_HALF + 1,
                      DPAD_ARM - 1, 2 * DPAD_HALF - 2, COL_HELD);
    }
    if (u) {
        gfx_fill_rect(DPAD_CX - DPAD_HALF + 1, DPAD_CY - DPAD_ARM - DPAD_HALF + 1,
                      2 * DPAD_HALF - 2, DPAD_ARM - 1, COL_HELD);
    }
    if (d) {
        gfx_fill_rect(DPAD_CX - DPAD_HALF + 1, DPAD_CY + DPAD_HALF,
                      2 * DPAD_HALF - 2, DPAD_ARM - 1, COL_HELD);
    }

    /* Repaint the hub in the bar colour so the cross reads as one piece: the
     * pressed-arm fills above stop at the hub edge, and without this the
     * seam between them shows. */
    gfx_fill_rect(DPAD_CX - DPAD_HALF + 1, DPAD_CY - DPAD_HALF + 1,
                  2 * DPAD_HALF - 2, 2 * DPAD_HALF - 2, COL_PAD);


    /* Arrows sit near the tip of each arm, pointing outward. */
    draw_arrow(DPAD_CX - DPAD_ARM - 4, DPAD_CY, 0, COL_TEXT);
    draw_arrow(DPAD_CX + DPAD_ARM + 4, DPAD_CY, 1, COL_TEXT);
    draw_arrow(DPAD_CX, DPAD_CY - DPAD_ARM - 4, 2, COL_TEXT);
    draw_arrow(DPAD_CX, DPAD_CY + DPAD_ARM + 4, 3, COL_TEXT);
}

/* Slightly darken a colour, used for the unpressed button body. */
static uint16_t dim(uint16_t c)
{
    int r = ((c >> 11) & 0x1F) * 3 / 4;
    int g = ((c >> 5)  & 0x3F) * 3 / 4;
    int b = (c         & 0x1F) * 3 / 4;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_face_buttons(uint32_t held)
{
    static const btn_id_t ids[4] = {
        BTN_ROT_CW, BTN_ROT_CCW, BTN_SOUND, BTN_HOLD
    };
    for (int i = 0; i < 4; i++) {
        const btn_rect_t *b = &UI_BUTTONS[ids[i]];
        int cx = b->x + b->w / 2;
        int cy = b->y + b->h / 2;
        int r  = b->w / 2;
        bool down = (held >> ids[i]) & 1u;

        gfx_disc(cx, cy, r, down ? b->tint : dim(b->tint));
        gfx_circle(cx, cy, r, COL_PAD_EDGE);

        int tw = gfx_text_width(b->label);
        gfx_text(cx - tw / 2, cy - FONT_ZH_SIZE / 2, b->label, COL_TEXT);
    }
}

static void draw_sys_buttons(uint32_t held, bool paused, bool sound_on)
{
    static const btn_id_t ids[2] = { BTN_PAUSE, BTN_RESTART };
    for (int i = 0; i < 2; i++) {
        const btn_rect_t *b = &UI_BUTTONS[ids[i]];
        bool down = (held >> ids[i]) & 1u;
        gfx_pill(b->x, b->y, b->w, b->h, down ? COL_HELD : COL_PAD);

        /* The pause button reads "繼續" once the game is actually paused. */
        const char *label = (ids[i] == BTN_PAUSE && paused) ? "繼續" : b->label;
        int tw = gfx_text_width(label);
        gfx_text(b->x + (b->w - tw) / 2,
                 b->y + (b->h - FONT_ZH_SIZE) / 2, label, COL_TEXT);
    }

    (void)sound_on;
}

/* Panel behind an overlay message. A flat box rather than a read-modify-write
 * dim: the field is redrawn in full each frame and never cleared, so dimming
 * what is already there would darken it further on every single frame. */
static void draw_overlay_box(int y, int h)
{
    int x = FIELD_X - 2;
    int w = FIELD_W + 4;
    gfx_fill_rect(x, y, w, h, RGB565(16, 18, 30));
    gfx_rect(x, y, w, h, COL_FRAME);
}

void ui_draw_static(void)
{
    gfx_clear(COL_BG);
    gfx_text_center(GFX_W / 2, 20, "俄羅斯方塊", COL_TEXT);
}

void ui_draw(const tetris_t *g, uint32_t held_mask, bool paused, bool sound_on)
{
    /* Nothing is cleared here on purpose. Blanking a region and painting it
     * back leaves the buffer briefly showing bare background, and the panel
     * scans continuously, so any frame it catches mid-redraw appears as a
     * flash. Every draw below fully overwrites the pixels it owns, so the
     * picture only ever transitions from one complete frame to the next.
     *
     * The numbers in the side panel are the one exception: they shrink when
     * the value gets shorter, so the digits are cleared individually there. */
    draw_field(g);
    draw_panel(g);
    draw_dpad(held_mask);
    draw_face_buttons(held_mask);
    draw_sys_buttons(held_mask, paused, sound_on);

    int fcx = FIELD_X + FIELD_W / 2;

    if (g->state == STATE_GAMEOVER) {
        draw_overlay_box(232, 190);
        gfx_text_center(fcx, 250, "遊戲結束", COL_TEXT);
        gfx_text_center(fcx, 300, "分數", COL_DIM);
        int nw = gfx_number_width(g->score);
        gfx_number(fcx - nw / 2, 334, g->score, COL_TEXT);
        gfx_text_center(fcx, 386, "按重來再玩一次", COL_DIM);
    } else if (paused) {
        draw_overlay_box(272, 120);
        gfx_text_center(fcx, 290, "暫停", COL_TEXT);
        gfx_text_center(fcx, 340, "按繼續回到遊戲", COL_DIM);
    }
}
