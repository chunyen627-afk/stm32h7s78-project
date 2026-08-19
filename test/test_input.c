/* Verifies touch rotation, button hit-testing and repeat timing. */
#include "input.h"
#include "ui.h"
#include "gfx.h"
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

/* The drawing map, duplicated here so the test is independent of gfx.c. */
static void portrait_to_physical(int x, int y, int *px, int *py)
{
    *px = y;
    *py = GFX_W - 1 - x;
}

static void test_rotation_is_inverse_of_drawing(void)
{
    /* For every portrait point, mapping to physical and back through the
     * touch rotation must return the original point. If these two disagree,
     * buttons respond in the wrong place on the panel. */
    int bad = 0;
    for (int y = 0; y < GFX_H; y += 7) {
        for (int x = 0; x < GFX_W; x += 5) {
            int px, py, rx, ry;
            portrait_to_physical(x, y, &px, &py);
            input_rotate_point((uint16_t)px, (uint16_t)py, &rx, &ry);
            if (rx != x || ry != y) {
                if (bad < 5) {
                    printf("  mismatch: (%d,%d) -> phys(%d,%d) -> (%d,%d)\n",
                           x, y, px, py, rx, ry);
                }
                bad++;
            }
        }
    }
    CHECK(bad == 0, "touch rotation inverts the drawing rotation exactly");
}

static void test_rotation_clamps(void)
{
    int x, y;
    input_rotate_point(0, 0, &x, &y);
    CHECK(x >= 0 && x < GFX_W && y >= 0 && y < GFX_H, "corner 0,0 in range");
    input_rotate_point(PHYS_W - 1, PHYS_H - 1, &x, &y);
    CHECK(x >= 0 && x < GFX_W && y >= 0 && y < GFX_H, "corner max in range");
    input_rotate_point(60000, 60000, &x, &y);
    CHECK(x >= 0 && x < GFX_W && y >= 0 && y < GFX_H, "wild input is clamped");
}

static void test_buttons_on_screen_and_disjoint(void)
{
    /* Every button must be fully on the canvas, below the playfield, and
     * must not overlap any other button. */
    for (int i = 0; i < BTN_COUNT; i++) {
        const btn_rect_t *b = &UI_BUTTONS[i];
        CHECK(b->x >= 0 && b->x + b->w <= GFX_W, "button within canvas width");
        CHECK(b->y >= 0 && b->y + b->h <= GFX_H, "button within canvas height");
        CHECK(b->y >= FIELD_Y + FIELD_H, "control sits below the playfield");
        /* D-pad arms are drawn as arrows and carry no text label. */
        CHECK(b->label != 0, "button has a label pointer");

        /* Two circular buttons can have overlapping bounding boxes while the
         * circles themselves stay apart, so compare centres for those. */
        for (int j = i + 1; j < BTN_COUNT; j++) {
            const btn_rect_t *c = &UI_BUTTONS[j];
            bool overlap;
            if (b->shape == SHAPE_CIRCLE && c->shape == SHAPE_CIRCLE) {
                int bx = b->x + b->w / 2, by = b->y + b->h / 2, br = b->w / 2;
                int cx = c->x + c->w / 2, cy = c->y + c->h / 2, cr = c->w / 2;
                int dx = bx - cx, dy = by - cy;
                overlap = (dx * dx + dy * dy) < (br + cr) * (br + cr);
            } else {
                overlap = !(b->x + b->w <= c->x || c->x + c->w <= b->x ||
                            b->y + b->h <= c->y || c->y + c->h <= b->y);
            }
            if (overlap) {
                printf("  buttons %d and %d overlap\n", i, j);
            }
            CHECK(!overlap, "buttons do not overlap");
        }
    }
}

static void test_hit_test_matches_rects(void)
{
    /* The centre of each button must hit that button, and points just
     * outside must not. */
    for (int i = 0; i < BTN_COUNT; i++) {
        const btn_rect_t *b = &UI_BUTTONS[i];
        CHECK(ui_hit_test(b->x + b->w / 2, b->y + b->h / 2) == (btn_id_t)i,
              "centre hits its own button");
        if (b->shape == SHAPE_RECT) {
            CHECK(ui_hit_test(b->x, b->y) == (btn_id_t)i, "top-left corner hits");
            CHECK(ui_hit_test(b->x + b->w - 1, b->y + b->h - 1) == (btn_id_t)i,
                  "bottom-right corner hits");
        } else if (b->shape == SHAPE_CIRCLE) {
            /* Corners lie outside a circular button by design. */
            CHECK(ui_hit_test(b->x, b->y) != (btn_id_t)i,
                  "circle corner is outside the button");
        }
        CHECK(ui_hit_test(b->x - 1, b->y + b->h / 2) != (btn_id_t)i,
              "just left of the button misses");
        CHECK(ui_hit_test(b->x + b->w, b->y + b->h / 2) != (btn_id_t)i,
              "just right of the button misses");
    }
    /* The playfield area must not report a button. */
    CHECK(ui_hit_test(FIELD_X + 10, FIELD_Y + 10) == BTN_COUNT,
          "playfield is not a button");
    CHECK(ui_hit_test(-5, -5) == BTN_COUNT, "off-canvas is not a button");
}

/* Press the centre of a button, feeding dt each frame. */
static btn_id_t press(input_t *in, tetris_t *g, btn_id_t id, uint32_t dt)
{
    const btn_rect_t *b = &UI_BUTTONS[id];
    return input_update(in, g, true, b->x + b->w / 2, b->y + b->h / 2, dt);
}

static void release(input_t *in, tetris_t *g, uint32_t dt)
{
    input_update(in, g, false, 0, 0, dt);
}

static void test_press_moves_piece(void)
{
    tetris_t g; input_t in;
    tetris_init(&g, 5);
    input_init(&in);
    memset(g.board, 0, sizeof(g.board));
    g.state = STATE_PLAYING;

    int x0 = g.cur.x;
    press(&in, &g, BTN_LEFT, 16);
    CHECK(g.cur.x == x0 - 1, "press fires immediately");

    /* Holding below the repeat delay must not move again. */
    press(&in, &g, BTN_LEFT, 50);
    CHECK(g.cur.x == x0 - 1, "no repeat before the delay");

    /* Past the delay it should repeat. */
    press(&in, &g, BTN_LEFT, REPEAT_DELAY_MS);
    CHECK(g.cur.x == x0 - 2, "repeats after the delay");

    release(&in, &g, 16);
    int x1 = g.cur.x;
    release(&in, &g, 1000);
    CHECK(g.cur.x == x1, "no movement while released");
}

static void test_rotate_does_not_repeat(void)
{
    tetris_t g; input_t in;
    tetris_init(&g, 6);
    input_init(&in);
    memset(g.board, 0, sizeof(g.board));
    g.cur.kind = PIECE_T;
    g.cur.rot  = 0;
    g.cur.x    = 4;
    g.cur.y    = 5;
    g.state    = STATE_PLAYING;

    press(&in, &g, BTN_ROT_CW, 16);
    int r1 = g.cur.rot;
    CHECK(r1 == 1, "rotate fires on press");

    /* Holding must not spin the piece continuously. */
    for (int i = 0; i < 20; i++) {
        press(&in, &g, BTN_ROT_CW, 50);
    }
    CHECK(g.cur.rot == r1, "rotation does not auto-repeat");
}

static void test_pause_toggles(void)
{
    tetris_t g; input_t in;
    tetris_init(&g, 8);
    input_init(&in);
    memset(g.board, 0, sizeof(g.board));
    g.state = STATE_PLAYING;

    CHECK(!in.paused, "starts unpaused");
    press(&in, &g, BTN_PAUSE, 16);
    CHECK(in.paused, "pause toggles on");
    release(&in, &g, 16);

    /* While paused, movement must be ignored. */
    int x0 = g.cur.x;
    press(&in, &g, BTN_LEFT, 16);
    CHECK(g.cur.x == x0, "movement ignored while paused");
    release(&in, &g, 16);

    press(&in, &g, BTN_PAUSE, 16);
    CHECK(!in.paused, "pause toggles off");
}

static void test_restart_button(void)
{
    tetris_t g; input_t in;
    tetris_init(&g, 13);
    input_init(&in);
    g.state = STATE_GAMEOVER;
    g.score = 4242;

    /* Pause must do nothing once the game is over. */
    press(&in, &g, BTN_PAUSE, 16);
    CHECK(g.state == STATE_GAMEOVER, "pause does not revive a finished game");
    CHECK(!in.paused, "pause ignored at game over");
    release(&in, &g, 16);

    /* Restart is its own button and works on a single press. */
    press(&in, &g, BTN_RESTART, 16);
    CHECK(g.state == STATE_PLAYING, "restart button starts a new game");
    CHECK(g.score == 0, "restart clears the score");
}

static void test_sound_toggle(void)
{
    tetris_t g; input_t in;
    tetris_init(&g, 19);
    input_init(&in);
    CHECK(in.sound_on, "sound starts on");

    press(&in, &g, BTN_SOUND, 16);
    CHECK(!in.sound_on, "X toggles sound off");
    release(&in, &g, 16);

    press(&in, &g, BTN_SOUND, 16);
    CHECK(in.sound_on, "X toggles sound back on");
    release(&in, &g, 16);

    /* Sound must still toggle while paused. */
    press(&in, &g, BTN_PAUSE, 16);  release(&in, &g, 16);
    CHECK(in.paused, "paused");
    bool before = in.sound_on;
    press(&in, &g, BTN_SOUND, 16);
    CHECK(in.sound_on != before, "sound toggles while paused");
}

static void test_slide_off_button_stops_repeat(void)
{
    tetris_t g; input_t in;
    tetris_init(&g, 17);
    input_init(&in);
    memset(g.board, 0, sizeof(g.board));
    g.state = STATE_PLAYING;

    press(&in, &g, BTN_LEFT, 16);
    int x0 = g.cur.x;
    /* Finger slides into the playfield: no further movement. */
    for (int i = 0; i < 20; i++) {
        input_update(&in, &g, true, FIELD_X + 5, FIELD_Y + 5, 50);
    }
    CHECK(g.cur.x == x0, "sliding off the button stops the action");
}

int main(void)
{
    printf("=== input / touch rotation tests ===\n");
    test_rotation_is_inverse_of_drawing();
    test_rotation_clamps();
    test_buttons_on_screen_and_disjoint();
    test_hit_test_matches_rects();
    test_press_moves_piece();
    test_rotate_does_not_repeat();
    test_pause_toggles();
    test_restart_button();
    test_sound_toggle();
    test_slide_off_button_stops_repeat();
    printf("=== %d checks, %d failures ===\n", checks, failures);
    printf(failures == 0 ? "ALL PASS\n" : "TESTS FAILED\n");
    return failures;
}
