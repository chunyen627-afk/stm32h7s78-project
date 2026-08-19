#include "input.h"

void input_init(input_t *in)
{
    in->active       = BTN_COUNT;
    in->held_ms      = 0;
    in->next_fire_ms = 0;
    in->paused       = false;
    in->sound_on     = true;
}

void input_rotate_point(uint16_t raw_x, uint16_t raw_y, int *out_x, int *out_y)
{
    /* Drawing maps portrait (x,y) -> physical (y, GFX_W-1-x).
     * Inverting: portrait x = GFX_W-1-physical_y, portrait y = physical_x. */
    int px = (int)raw_x;
    int py = (int)raw_y;

    if (px < 0) px = 0;
    if (px > PHYS_W - 1) px = PHYS_W - 1;
    if (py < 0) py = 0;
    if (py > PHYS_H - 1) py = PHYS_H - 1;

    *out_x = (GFX_W - 1) - py;
    *out_y = px;
}

/* Buttons that make sense to auto-repeat while held. */
static bool repeats(btn_id_t b)
{
    return b == BTN_LEFT || b == BTN_RIGHT || b == BTN_SOFT;
}

static void apply(tetris_t *g, btn_id_t b)
{
    switch (b) {
        case BTN_LEFT:    tetris_move(g, -1);   break;
        case BTN_RIGHT:   tetris_move(g, 1);    break;
        case BTN_ROT_CW:  tetris_rotate(g, 1);  break;
        case BTN_ROT_CCW: tetris_rotate(g, -1); break;
        case BTN_SOFT:    tetris_soft_drop(g);  break;
        case BTN_HARD:    tetris_hard_drop(g);  break;
        case BTN_HOLD:    tetris_hold(g);       break;
        default: break;
    }
}

btn_id_t input_update(input_t *in, tetris_t *g,
                      bool touched, int px, int py, uint32_t dt_ms)
{
    btn_id_t hit = touched ? ui_hit_test(px, py) : BTN_COUNT;

    /* New press, or the finger moved onto a different button. */
    if (hit != in->active) {
        in->active       = hit;
        in->held_ms      = 0;
        in->next_fire_ms = REPEAT_DELAY_MS;

        if (hit == BTN_COUNT) {
            return BTN_COUNT;
        }

        /* These three work regardless of pause or game-over state. */
        switch (hit) {
            case BTN_SOUND:
                in->sound_on = !in->sound_on;
                return hit;
            case BTN_RESTART:
                tetris_restart(g);
                in->paused = false;
                return hit;
            case BTN_PAUSE:
                if (g->state != STATE_GAMEOVER) {
                    in->paused = !in->paused;
                }
                return hit;
            default:
                break;
        }

        /* Everything else only acts while actually playing. */
        if (g->state == STATE_PLAYING && !in->paused) {
            apply(g, hit);
        }
        return hit;
    }

    if (hit == BTN_COUNT) {
        return BTN_COUNT;
    }

    in->held_ms += dt_ms;

    if (g->state != STATE_PLAYING || in->paused) {
        return hit;
    }

    /* Auto-repeat for the movement keys. */
    if (repeats(hit) && in->held_ms >= in->next_fire_ms) {
        apply(g, hit);
        in->next_fire_ms = in->held_ms + REPEAT_RATE_MS;
    }
    return hit;
}
