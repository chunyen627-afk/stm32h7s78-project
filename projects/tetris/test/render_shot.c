/* Renders game screens into the framebuffer and dumps them as raw RGB565
 * over semihosting, so the layout can be inspected without hardware. */
#include "ui.h"
#include "input.h"
#include "gfx.h"
#include <stdio.h>
#include <string.h>

static uint16_t fb[PHYS_W * PHYS_H];

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("could not open %s\n", path);
        return;
    }
    fwrite(fb, 2, (size_t)PHYS_W * PHYS_H, f);
    fclose(f);
    printf("wrote %s\n", path);
}

/* Play a scripted game so the shot shows a realistic stack. */
static void build_position(tetris_t *g)
{
    tetris_init(g, 2024);
    memset(g->board, 0, sizeof(g->board));

    /* A ragged stack near the bottom. */
    static const uint8_t heights[TET_W] = { 4, 6, 3, 7, 5, 2, 6, 4, 3, 0 };
    static const uint8_t kinds[]  = { CELL_I, CELL_J, CELL_L, CELL_O,
                                      CELL_S, CELL_T, CELL_Z };
    for (int x = 0; x < TET_W; x++) {
        for (int h = 0; h < heights[x]; h++) {
            int y = TET_TOTAL_H - 1 - h;
            g->board[y][x] = kinds[(x + h) % 7];
        }
    }

    g->cur.kind = PIECE_T;
    g->cur.rot  = 0;
    g->cur.x    = 3;
    g->cur.y    = 4;
    g->next     = PIECE_I;
    g->hold     = PIECE_L;
    g->has_hold = true;
    g->score    = 12480;
    g->level    = 3;
    g->lines    = 37;
    g->state    = STATE_PLAYING;
}

int main(void)
{
    tetris_t g;
    gfx_set_framebuffer(fb);
    ui_draw_static();

    /* 1: normal play. */
    build_position(&g);
    ui_draw(&g, 0, false, true);
    dump("shot_play.raw");

    /* 2: a button held down. */
    ui_draw(&g, (1u<<BTN_LEFT)|(1u<<BTN_ROT_CW), false, true);
    dump("shot_held.raw");

    /* 3: paused. */
    ui_draw(&g, 0, true, true);
    dump("shot_pause.raw");

    /* 4: line clear flashing. */
    build_position(&g);
    for (int x = 0; x < TET_W; x++) {
        g.board[TET_TOTAL_H - 1][x] = CELL_O;
        g.board[TET_TOTAL_H - 2][x] = CELL_S;
    }
    g.state = STATE_CLEARING;
    g.clearing_count = 2;
    g.clearing_rows[0] = TET_TOTAL_H - 1;
    g.clearing_rows[1] = TET_TOTAL_H - 2;
    ui_draw(&g, 0, false, true);
    dump("shot_clear.raw");

    /* 5: game over. */
    build_position(&g);
    g.state = STATE_GAMEOVER;
    ui_draw(&g, 0, false, true);
    dump("shot_over.raw");

    printf("done\n");
    return 0;
}
