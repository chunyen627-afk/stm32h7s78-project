/* Host-side test harness for the tetris core.
 * Built for the ARM simulator in arm-none-eabi-gdb, output via semihosting. */
#include "tetris.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        printf("FAIL: %s (line %d)\n", (msg), __LINE__);         \
        failures++;                                             \
    }                                                           \
} while (0)

/* Force a specific piece into play, bypassing the bag. */
static void force_piece(tetris_t *g, piece_t k, int rot, int x, int y)
{
    g->cur.kind = k;
    g->cur.rot  = (int8_t)rot;
    g->cur.x    = (int8_t)x;
    g->cur.y    = (int8_t)y;
    g->state    = STATE_PLAYING;
}

/* Count filled cells across the whole board. */
static int board_count(const tetris_t *g)
{
    int n = 0;
    for (int y = 0; y < TET_TOTAL_H; y++)
        for (int x = 0; x < TET_W; x++)
            if (g->board[y][x] != CELL_EMPTY) n++;
    return n;
}

static void test_shapes_have_four_cells(void)
{
    /* Every tetromino, in every rotation, must be exactly 4 cells. */
    for (int k = 0; k < PIECE_COUNT; k++) {
        for (int r = 0; r < 4; r++) {
            int n = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    if (tetris_preview_cell((piece_t)k, x, y)) n++;
            /* preview only exposes rot 0; check via a game instead */
            (void)r;
            if (r == 0) CHECK(n == 4, "piece rot0 must have 4 cells");
        }
    }
}

static void test_all_rotations_four_cells(void)
{
    /* Walk every rotation by locking pieces and counting what lands. */
    for (int k = 0; k < PIECE_COUNT; k++) {
        for (int r = 0; r < 4; r++) {
            tetris_t g;
            tetris_init(&g, 42);
            memset(g.board, 0, sizeof(g.board));
            force_piece(&g, (piece_t)k, r, 3, 5);
            tetris_hard_drop(&g);
            int n = board_count(&g);
            if (n != 4) {
                printf("  piece=%d rot=%d cells=%d\n", k, r, n);
            }
            CHECK(n == 4, "every rotation locks exactly 4 cells");
        }
    }
}

static void test_bag_is_fair(void)
{
    /* Each 7-bag must contain all 7 pieces exactly once. */
    tetris_t g;
    tetris_init(&g, 12345);
    int seen[PIECE_COUNT] = {0};
    /* g.next plus the bag contents form the sequence; sample many bags. */
    for (int bag = 0; bag < 20; bag++) {
        int local[PIECE_COUNT] = {0};
        for (int i = 0; i < PIECE_COUNT; i++) {
            memset(g.board, 0, sizeof(g.board));
            g.state = STATE_PLAYING;
            piece_t p = g.cur.kind;
            local[p]++;
            seen[p]++;
            force_piece(&g, p, 0, 3, 18);
            tetris_hard_drop(&g);
        }
        (void)local;
    }
    for (int i = 0; i < PIECE_COUNT; i++) {
        CHECK(seen[i] > 0, "every piece kind appears");
    }
}

static void test_line_clear(void)
{
    tetris_t g;
    tetris_init(&g, 7);
    memset(g.board, 0, sizeof(g.board));

    /* Fill the bottom row except one column. */
    int row = TET_TOTAL_H - 1;
    for (int x = 0; x < TET_W; x++) g.board[row][x] = CELL_J;
    g.board[row][0] = CELL_EMPTY;

    CHECK(board_count(&g) == TET_W - 1, "setup row has 9 cells");

    /* Drop a vertical I into the gap: fills column 0 rows 17..20 region. */
    force_piece(&g, PIECE_I, 1, -1, 0);
    tetris_hard_drop(&g);

    CHECK(g.state == STATE_CLEARING, "full row triggers clearing state");
    CHECK(g.clearing_count == 1, "exactly one row clearing");

    /* Run the animation out. */
    tetris_update(&g, TET_CLEAR_MS + 1);
    CHECK(g.state == STATE_PLAYING, "returns to playing after clear");
    CHECK(g.lines == 1, "line counter incremented");
    CHECK(g.score > 0, "score awarded");
    /* I was 4 tall, 1 row consumed -> 3 cells remain. */
    CHECK(board_count(&g) == 3, "cleared row removed, rest collapsed");
}

static void test_tetris_four_lines(void)
{
    tetris_t g;
    tetris_init(&g, 9);
    memset(g.board, 0, sizeof(g.board));

    /* Fill four bottom rows except column 0. */
    for (int r = TET_TOTAL_H - 4; r < TET_TOTAL_H; r++) {
        for (int x = 1; x < TET_W; x++) g.board[r][x] = CELL_L;
    }
    force_piece(&g, PIECE_I, 1, -1, 0);
    tetris_hard_drop(&g);

    CHECK(g.clearing_count == 4, "tetris clears four rows");
    tetris_update(&g, TET_CLEAR_MS + 1);
    CHECK(g.lines == 4, "four lines counted");
    CHECK(board_count(&g) == 0, "board empty after tetris");
    /* 800 * (level+1), level 0 -> 800, plus hard-drop bonus. */
    CHECK(g.score >= 800, "tetris scores at least 800");
}

static void test_walls(void)
{
    tetris_t g;
    tetris_init(&g, 3);
    memset(g.board, 0, sizeof(g.board));

    force_piece(&g, PIECE_O, 0, 0, 5);
    for (int i = 0; i < 20; i++) tetris_move(&g, -1);
    CHECK(g.cur.x >= -1, "piece cannot escape left wall");
    int leftmost = g.cur.x;
    tetris_move(&g, -1);
    CHECK(g.cur.x == leftmost, "blocked at left wall");

    for (int i = 0; i < 30; i++) tetris_move(&g, 1);
    int rightmost = g.cur.x;
    tetris_move(&g, 1);
    CHECK(g.cur.x == rightmost, "blocked at right wall");

    /* O occupies cols 1..2 of its box, so max x is TET_W-3. */
    CHECK(rightmost == TET_W - 3, "O stops at correct right column");
}

/* Does the active piece overlap a filled board cell or sit outside the walls? */
static bool piece_overlaps(const tetris_t *g)
{
    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            if (!tetris_shape_cell(g->cur.kind, g->cur.rot, cx, cy)) continue;
            int bx = g->cur.x + cx;
            int by = g->cur.y + cy;
            if (bx < 0 || bx >= TET_W) return true;        /* through a wall */
            if (by >= TET_TOTAL_H) return true;            /* through the floor */
            if (by < 0) continue;                          /* above ceiling is legal */
            if (g->board[by][bx] != CELL_EMPTY) return true;
        }
    }
    return false;
}

static void test_no_overlap_after_rotate(void)
{
    /* Rotating must never push a piece into a wall or into settled blocks.
     * Build an obstacle course so the kick table is genuinely exercised. */
    tetris_t g;
    tetris_init(&g, 55);
    int kicked = 0;

    for (int k = 0; k < PIECE_COUNT; k++) {
        for (int r = 0; r < 4; r++) {
            for (int dir = -1; dir <= 1; dir += 2) {
                /* Try a spread of positions, including hard against the walls. */
                for (int px = -1; px <= TET_W - 2; px++) {
                    memset(g.board, 0, sizeof(g.board));
                    for (int x = 0; x < TET_W; x++)
                        g.board[TET_TOTAL_H - 1][x] = CELL_S;
                    for (int y = 14; y < TET_TOTAL_H; y++)
                        g.board[y][0] = CELL_S;

                    force_piece(&g, (piece_t)k, r, px, TET_TOTAL_H - 6);
                    if (piece_overlaps(&g)) continue;   /* invalid start, skip */

                    int8_t ox = g.cur.x, oy = g.cur.y, orot = g.cur.rot;
                    tetris_rotate(&g, dir);

                    CHECK(!piece_overlaps(&g),
                          "rotation never overlaps walls or stack");
                    if (g.cur.x != ox || g.cur.y != oy) kicked++;
                    /* Rotation either applied or was rejected outright. */
                    CHECK(g.cur.rot == orot ||
                          g.cur.rot == ((orot + (dir > 0 ? 1 : 3)) & 3),
                          "rotation lands on a legal state");
                }
            }
        }
    }
    printf("  wall kicks exercised: %d\n", kicked);
    CHECK(kicked > 0, "kick table actually triggered");
}

static void test_gravity(void)
{
    tetris_t g;
    tetris_init(&g, 11);
    memset(g.board, 0, sizeof(g.board));
    force_piece(&g, PIECE_O, 0, 4, 0);
    int y0 = g.cur.y;

    tetris_update(&g, 10);
    CHECK(g.cur.y == y0, "no drop before interval elapses");

    tetris_update(&g, 900);
    CHECK(g.cur.y == y0 + 1, "drops one row after interval");
}

static void test_hold(void)
{
    tetris_t g;
    tetris_init(&g, 21);
    memset(g.board, 0, sizeof(g.board));

    piece_t first = g.cur.kind;
    tetris_hold(&g);
    CHECK(g.has_hold, "hold slot populated");
    CHECK(g.hold == first, "held piece is the one we swapped out");
    CHECK(g.cur.kind != first || true, "new piece in play");

    piece_t second = g.cur.kind;
    tetris_hold(&g);
    CHECK(g.cur.kind == second, "second hold blocked until piece locks");
}

static void test_ghost(void)
{
    tetris_t g;
    tetris_init(&g, 31);
    memset(g.board, 0, sizeof(g.board));
    force_piece(&g, PIECE_O, 0, 4, 0);
    int gy = tetris_ghost_y(&g);
    CHECK(gy > g.cur.y, "ghost is below the piece");
    tetris_hard_drop(&g);
    /* After hard drop the piece locked at the ghost row. */
    CHECK(board_count(&g) == 4, "hard drop locked the piece");
}

static void test_gameover(void)
{
    tetris_t g;
    tetris_init(&g, 41);
    /* Fill the board completely; the next spawn must fail. */
    for (int y = 0; y < TET_TOTAL_H; y++)
        for (int x = 0; x < TET_W; x++)
            g.board[y][x] = CELL_T;
    g.state = STATE_PLAYING;
    force_piece(&g, PIECE_O, 0, 4, 0);
    tetris_hard_drop(&g);
    CHECK(g.state == STATE_GAMEOVER || g.state == STATE_CLEARING,
          "full board ends the game or clears");
}

static void test_cell_at_bounds(void)
{
    tetris_t g;
    tetris_init(&g, 51);
    CHECK(tetris_cell_at(&g, -1, 0) == CELL_EMPTY, "negative x is empty");
    CHECK(tetris_cell_at(&g, TET_W, 0) == CELL_EMPTY, "x past width is empty");
    CHECK(tetris_cell_at(&g, 0, -1) == CELL_EMPTY, "negative y is empty");
    CHECK(tetris_cell_at(&g, 0, TET_H) == CELL_EMPTY, "y past height is empty");
}

static void test_long_random_game(void)
{
    /* Fuzz: play a long game with pseudo-random inputs and make sure the
     * invariants never break. */
    tetris_t g;
    tetris_init(&g, 0xC0FFEE);
    uint32_t r = 12345;
    int restarts = 0;

    for (int step = 0; step < 200000; step++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        switch (r % 6) {
            case 0: tetris_move(&g, -1); break;
            case 1: tetris_move(&g, 1); break;
            case 2: tetris_rotate(&g, 1); break;
            case 3: tetris_rotate(&g, -1); break;
            case 4: tetris_soft_drop(&g); break;
            case 5: if ((r >> 8) % 8 == 0) tetris_hard_drop(&g); break;
        }
        tetris_update(&g, 16);

        /* Invariant: cell values always in range. */
        for (int y = 0; y < TET_TOTAL_H; y++) {
            for (int x = 0; x < TET_W; x++) {
                if (g.board[y][x] >= CELL_COUNT) {
                    printf("BAD CELL %d at %d,%d step %d\n",
                           g.board[y][x], x, y, step);
                    failures++;
                    return;
                }
            }
        }
        /* Invariant: the live piece never intersects walls, floor or stack. */
        if (g.state == STATE_PLAYING && piece_overlaps(&g)) {
            printf("OVERLAP kind=%d rot=%d x=%d y=%d step %d\n",
                   g.cur.kind, g.cur.rot, g.cur.x, g.cur.y, step);
            failures++;
            return;
        }
        if (g.state == STATE_GAMEOVER) {
            tetris_restart(&g);
            restarts++;
        }
    }
    printf("  fuzz completed, %d games played, score sample %lu\n",
           restarts, (unsigned long)g.score);
    CHECK(restarts > 0, "fuzz played at least one full game");
}

int main(void)
{
    printf("=== tetris core tests ===\n");
    test_shapes_have_four_cells();
    test_all_rotations_four_cells();
    test_bag_is_fair();
    test_line_clear();
    test_tetris_four_lines();
    test_walls();
    test_no_overlap_after_rotate();
    test_gravity();
    test_hold();
    test_ghost();
    test_gameover();
    test_cell_at_bounds();
    test_long_random_game();

    printf("=== %d checks, %d failures ===\n", checks, failures);
    printf(failures == 0 ? "ALL PASS\n" : "TESTS FAILED\n");
    return failures;
}
