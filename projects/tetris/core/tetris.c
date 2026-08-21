#include "tetris.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Piece shapes.
 *
 * Each piece lives in a 4x4 box, stored as a 16-bit mask, bit (row*4+col),
 * bit 0 = top-left. Four rotation states, in SRS order (0=spawn, 1=CW,
 * 2=180, 3=CCW).
 * ------------------------------------------------------------------------- */
static const uint16_t SHAPES[PIECE_COUNT][4] = {
    /* I */ { 0x0F00, 0x2222, 0x00F0, 0x4444 },
    /* J */ { 0x8E00, 0x6440, 0x0E20, 0x44C0 },
    /* L */ { 0x2E00, 0x4460, 0x0E80, 0xC440 },
    /* O */ { 0x6600, 0x6600, 0x6600, 0x6600 },
    /* S */ { 0x6C00, 0x4620, 0x06C0, 0x8C40 },
    /* T */ { 0x4E00, 0x4640, 0x0E40, 0x4C40 },
    /* Z */ { 0xC600, 0x2640, 0x0C60, 0x4C80 },
};

static const cell_t PIECE_CELL[PIECE_COUNT] = {
    CELL_I, CELL_J, CELL_L, CELL_O, CELL_S, CELL_T, CELL_Z
};

/* SRS wall kicks. Offsets tried in order; first that fits wins.
 * Indexed [from_rot][kick_index], as (dx, dy) with dy positive = down. */
static const int8_t KICKS_JLSTZ[4][5][2] = {
    /* 0->1 */ { {0,0}, {-1,0}, {-1,-1}, {0,2}, {-1,2} },
    /* 1->2 */ { {0,0}, {1,0},  {1,1},   {0,-2},{1,-2} },
    /* 2->3 */ { {0,0}, {1,0},  {1,-1},  {0,2}, {1,2}  },
    /* 3->0 */ { {0,0}, {-1,0}, {-1,1},  {0,-2},{-1,-2}},
};
static const int8_t KICKS_I[4][5][2] = {
    /* 0->1 */ { {0,0}, {-2,0}, {1,0},  {-2,1}, {1,-2} },
    /* 1->2 */ { {0,0}, {-1,0}, {2,0},  {-1,-2},{2,1}  },
    /* 2->3 */ { {0,0}, {2,0},  {-1,0}, {2,-1}, {-1,2} },
    /* 3->0 */ { {0,0}, {1,0},  {-2,0}, {1,2},  {-2,-1}},
};

/* Gravity interval per level, in milliseconds. Classic-style curve. */
static const uint16_t FALL_MS[] = {
    800, 720, 630, 550, 470, 380, 300, 220, 130, 100,
     80,  80,  80,  70,  70,  70,  50,  50,  50,  30
};
#define MAX_LEVEL ((int)(sizeof(FALL_MS)/sizeof(FALL_MS[0])) - 1)

static uint32_t rng_next(tetris_t *g)
{
    /* xorshift32 - small and good enough for piece order. */
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

/* Refill and shuffle the 7-bag. */
static void bag_refill(tetris_t *g)
{
    for (int i = 0; i < PIECE_COUNT; i++) {
        g->bag[i] = (uint8_t)i;
    }
    for (int i = PIECE_COUNT - 1; i > 0; i--) {
        uint32_t j = rng_next(g) % (uint32_t)(i + 1);
        uint8_t t = g->bag[i];
        g->bag[i] = g->bag[(int)j];
        g->bag[(int)j] = t;
    }
    g->bag_pos = 0;
}

static piece_t bag_take(tetris_t *g)
{
    if (g->bag_pos >= PIECE_COUNT) {
        bag_refill(g);
    }
    return (piece_t)g->bag[g->bag_pos++];
}

static bool shape_bit(piece_t kind, int rot, int cx, int cy)
{
    if (cx < 0 || cx > 3 || cy < 0 || cy > 3) {
        return false;
    }
    return (SHAPES[kind][rot & 3] >> (cy * 4 + cx)) & 1u;
}

/* Can the piece sit at this position without overlapping walls or stack? */
static bool fits(const tetris_t *g, piece_t kind, int rot, int px, int py)
{
    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            if (!shape_bit(kind, rot, cx, cy)) {
                continue;
            }
            int bx = px + cx;
            int by = py + cy;
            if (bx < 0 || bx >= TET_W || by >= TET_TOTAL_H) {
                return false;
            }
            /* Above the ceiling is allowed - pieces spawn partly off-screen. */
            if (by < 0) {
                continue;
            }
            if (g->board[by][bx] != CELL_EMPTY) {
                return false;
            }
        }
    }
    return true;
}

static void spawn(tetris_t *g)
{
    g->cur.kind = g->next;
    g->next     = bag_take(g);
    g->cur.rot  = 0;
    g->cur.x    = 3;
    g->cur.y    = 0;
    g->hold_used = false;
    g->fall_timer_ms = 0;

    /* If the new piece cannot be placed, the stack has reached the top. */
    if (!fits(g, g->cur.kind, g->cur.rot, g->cur.x, g->cur.y)) {
        g->state = STATE_GAMEOVER;
    }
}

/* Write the active piece into the board. */
static void lock_piece(tetris_t *g)
{
    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            if (!shape_bit(g->cur.kind, g->cur.rot, cx, cy)) {
                continue;
            }
            int bx = g->cur.x + cx;
            int by = g->cur.y + cy;
            if (by >= 0 && by < TET_TOTAL_H && bx >= 0 && bx < TET_W) {
                g->board[by][bx] = (uint8_t)PIECE_CELL[g->cur.kind];
            }
        }
    }
}

/* Find full rows and start the clear animation. Returns how many were found. */
static int scan_full_rows(tetris_t *g)
{
    g->clearing_count = 0;
    for (int y = 0; y < TET_TOTAL_H; y++) {
        bool full = true;
        for (int x = 0; x < TET_W; x++) {
            if (g->board[y][x] == CELL_EMPTY) {
                full = false;
                break;
            }
        }
        if (full && g->clearing_count < 4) {
            g->clearing_rows[g->clearing_count++] = (uint8_t)y;
        }
    }
    return g->clearing_count;
}

/* Remove the rows recorded in clearing_rows and collapse the stack. */
static void collapse_rows(tetris_t *g)
{
    for (int i = 0; i < g->clearing_count; i++) {
        int row = g->clearing_rows[i];
        for (int y = row; y > 0; y--) {
            memcpy(g->board[y], g->board[y - 1], TET_W);
        }
        memset(g->board[0], CELL_EMPTY, TET_W);
    }

    static const uint16_t LINE_SCORE[5] = { 0, 100, 300, 500, 800 };
    int n = g->clearing_count;
    if (n > 4) {
        n = 4;
    }
    g->score += (uint32_t)LINE_SCORE[n] * (g->level + 1);
    g->lines += (uint32_t)n;
    g->level  = g->lines / 10;
    if (g->level > (uint32_t)MAX_LEVEL) {
        g->level = (uint32_t)MAX_LEVEL;
    }
    g->clearing_count = 0;
}

/* Lock the piece, resolve line clears, and bring in the next piece. */
static void lock_and_next(tetris_t *g)
{
    lock_piece(g);
    if (scan_full_rows(g) > 0) {
        g->state = STATE_CLEARING;
        g->clear_timer_ms = 0;
    } else {
        spawn(g);
    }
}

void tetris_init(tetris_t *g, uint32_t seed)
{
    memset(g, 0, sizeof(*g));
    g->rng = seed ? seed : 0x1234567u;
    bag_refill(g);
    g->next     = bag_take(g);
    g->hold     = PIECE_COUNT;   /* sentinel: nothing held */
    g->has_hold = false;
    g->state    = STATE_PLAYING;
    g->level    = 0;
    spawn(g);
}

void tetris_restart(tetris_t *g)
{
    tetris_init(g, g->rng ^ 0x9E3779B9u);
}

void tetris_move(tetris_t *g, int dx)
{
    if (g->state != STATE_PLAYING) {
        return;
    }
    if (fits(g, g->cur.kind, g->cur.rot, g->cur.x + dx, g->cur.y)) {
        g->cur.x = (int8_t)(g->cur.x + dx);
    }
}

void tetris_rotate(tetris_t *g, int dir)
{
    if (g->state != STATE_PLAYING) {
        return;
    }
    /* O never needs to move. */
    if (g->cur.kind == PIECE_O) {
        return;
    }

    int from = g->cur.rot & 3;
    int to   = (from + (dir > 0 ? 1 : 3)) & 3;

    /* Kick table is indexed by the transition. Going CCW is the reverse of
     * the CW transition out of the destination state, with offsets negated. */
    const int8_t (*table)[5][2] =
        (g->cur.kind == PIECE_I) ? KICKS_I : KICKS_JLSTZ;
    int  idx  = (dir > 0) ? from : to;
    int  sign = (dir > 0) ? 1 : -1;

    for (int k = 0; k < 5; k++) {
        int dx = sign * table[idx][k][0];
        int dy = sign * table[idx][k][1];
        if (fits(g, g->cur.kind, to, g->cur.x + dx, g->cur.y + dy)) {
            g->cur.rot = (int8_t)to;
            g->cur.x   = (int8_t)(g->cur.x + dx);
            g->cur.y   = (int8_t)(g->cur.y + dy);
            return;
        }
    }
}

void tetris_soft_drop(tetris_t *g)
{
    if (g->state != STATE_PLAYING) {
        return;
    }
    if (fits(g, g->cur.kind, g->cur.rot, g->cur.x, g->cur.y + 1)) {
        g->cur.y = (int8_t)(g->cur.y + 1);
        g->score += 1;
        g->fall_timer_ms = 0;
    } else {
        lock_and_next(g);
    }
}

void tetris_hard_drop(tetris_t *g)
{
    if (g->state != STATE_PLAYING) {
        return;
    }
    int dropped = 0;
    while (fits(g, g->cur.kind, g->cur.rot, g->cur.x, g->cur.y + 1)) {
        g->cur.y = (int8_t)(g->cur.y + 1);
        dropped++;
    }
    g->score += (uint32_t)dropped * 2u;
    lock_and_next(g);
}

void tetris_hold(tetris_t *g)
{
    if (g->state != STATE_PLAYING || g->hold_used) {
        return;
    }
    piece_t incoming;
    if (g->has_hold) {
        incoming = g->hold;
        g->hold  = g->cur.kind;
    } else {
        incoming = g->next;
        g->next  = bag_take(g);
        g->hold  = g->cur.kind;
        g->has_hold = true;
    }

    g->cur.kind = incoming;
    g->cur.rot  = 0;
    g->cur.x    = 3;
    g->cur.y    = 0;
    g->hold_used = true;
    g->fall_timer_ms = 0;

    if (!fits(g, g->cur.kind, g->cur.rot, g->cur.x, g->cur.y)) {
        g->state = STATE_GAMEOVER;
    }
}

void tetris_update(tetris_t *g, uint32_t dt_ms)
{
    if (g->state == STATE_CLEARING) {
        g->clear_timer_ms += dt_ms;
        if (g->clear_timer_ms >= TET_CLEAR_MS) {
            collapse_rows(g);
            g->state = STATE_PLAYING;
            spawn(g);
        }
        return;
    }

    if (g->state != STATE_PLAYING) {
        return;
    }

    uint32_t interval = FALL_MS[g->level > (uint32_t)MAX_LEVEL
                                ? (uint32_t)MAX_LEVEL : g->level];
    g->fall_timer_ms += dt_ms;
    while (g->fall_timer_ms >= interval && g->state == STATE_PLAYING) {
        g->fall_timer_ms -= interval;
        if (fits(g, g->cur.kind, g->cur.rot, g->cur.x, g->cur.y + 1)) {
            g->cur.y = (int8_t)(g->cur.y + 1);
        } else {
            lock_and_next(g);
            break;
        }
    }
}

int tetris_ghost_y(const tetris_t *g)
{
    int y = g->cur.y;
    while (fits(g, g->cur.kind, g->cur.rot, g->cur.x, y + 1)) {
        y++;
    }
    return y;
}

uint8_t tetris_cell_at(const tetris_t *g, int x, int y)
{
    if (x < 0 || x >= TET_W || y < 0 || y >= TET_H) {
        return CELL_EMPTY;
    }
    /* Visible coordinates start below the hidden spawn rows. */
    int by = y + TET_HIDDEN;

    if (g->state == STATE_PLAYING) {
        int rx = x - g->cur.x;
        int ry = by - g->cur.y;
        if (shape_bit(g->cur.kind, g->cur.rot, rx, ry)) {
            return (uint8_t)PIECE_CELL[g->cur.kind];
        }
    }
    return g->board[by][x];
}

bool tetris_row_clearing(const tetris_t *g, int y)
{
    if (g->state != STATE_CLEARING) {
        return false;
    }
    int by = y + TET_HIDDEN;
    for (int i = 0; i < g->clearing_count; i++) {
        if (g->clearing_rows[i] == by) {
            return true;
        }
    }
    return false;
}

bool tetris_preview_cell(piece_t kind, int x, int y)
{
    if (kind >= PIECE_COUNT) {
        return false;
    }
    return shape_bit(kind, 0, x, y);
}

bool tetris_shape_cell(piece_t kind, int rot, int x, int y)
{
    if (kind >= PIECE_COUNT) {
        return false;
    }
    return shape_bit(kind, rot, x, y);
}
