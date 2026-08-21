/**
 * Tetris core logic - hardware independent.
 * Board is 10 wide x 20 tall, played in portrait orientation.
 */
#ifndef TETRIS_H
#define TETRIS_H

#include <stdint.h>
#include <stdbool.h>

#define TET_W 10
#define TET_H 20
/* Hidden rows above the visible field where pieces spawn. */
#define TET_HIDDEN 2
#define TET_TOTAL_H (TET_H + TET_HIDDEN)

typedef enum {
    CELL_EMPTY = 0,
    CELL_I, CELL_J, CELL_L, CELL_O, CELL_S, CELL_T, CELL_Z,
    CELL_COUNT
} cell_t;

typedef enum {
    PIECE_I = 0, PIECE_J, PIECE_L, PIECE_O, PIECE_S, PIECE_T, PIECE_Z,
    PIECE_COUNT
} piece_t;

typedef enum {
    STATE_PLAYING = 0,
    STATE_CLEARING,
    STATE_GAMEOVER
} game_state_t;

typedef struct {
    piece_t kind;
    int8_t  rot;   /* 0..3 */
    int8_t  x;     /* column of piece origin */
    int8_t  y;     /* row of piece origin, counted from top of TET_TOTAL_H */
} active_piece_t;

typedef struct {
    uint8_t        board[TET_TOTAL_H][TET_W];
    active_piece_t cur;
    piece_t        next;
    piece_t        hold;
    bool           has_hold;
    bool           hold_used;   /* only one hold per piece */

    game_state_t   state;
    uint32_t       score;
    uint32_t       lines;
    uint32_t       level;

    /* Gravity: accumulates elapsed ms, drops when it exceeds the interval. */
    uint32_t       fall_timer_ms;

    /* Line-clear animation. */
    uint8_t        clearing_rows[4];
    uint8_t        clearing_count;
    uint32_t       clear_timer_ms;

    /* 7-bag randomiser. */
    uint8_t        bag[PIECE_COUNT];
    uint8_t        bag_pos;
    uint32_t       rng;
} tetris_t;

/* Milliseconds the line-clear flash lasts. */
#define TET_CLEAR_MS 220u

void     tetris_init(tetris_t *g, uint32_t seed);
/* Advance the game by dt_ms. Call once per frame. */
void     tetris_update(tetris_t *g, uint32_t dt_ms);

void     tetris_move(tetris_t *g, int dx);
void     tetris_rotate(tetris_t *g, int dir);   /* dir = +1 CW, -1 CCW */
void     tetris_soft_drop(tetris_t *g);
void     tetris_hard_drop(tetris_t *g);
void     tetris_hold(tetris_t *g);
void     tetris_restart(tetris_t *g);

/* Cell value for rendering, including the active piece. Returns CELL_EMPTY
 * for out-of-range coordinates. y is in visible-field coordinates (0..TET_H-1). */
uint8_t  tetris_cell_at(const tetris_t *g, int x, int y);
/* Ghost-piece row: where the current piece would land. */
int      tetris_ghost_y(const tetris_t *g);
/* 4x4 preview grid for a piece kind, used to draw next/hold boxes. */
bool     tetris_preview_cell(piece_t kind, int x, int y);
/* 4x4 grid for an arbitrary rotation state, used by tests and rendering. */
bool     tetris_shape_cell(piece_t kind, int rot, int x, int y);
/* True while a cleared row should be drawn flashing. */
bool     tetris_row_clearing(const tetris_t *g, int y);

#endif /* TETRIS_H */
