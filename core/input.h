/**
 * Turns raw touch samples into game actions.
 *
 * The panel reports landscape coordinates; this layer rotates them into the
 * 480x800 portrait space the UI is laid out in, then applies press/repeat
 * timing so holding a button auto-repeats like a keyboard.
 */
#ifndef INPUT_H_INCLUDED
#define INPUT_H_INCLUDED

#include "tetris.h"
#include "ui.h"
#include <stdbool.h>
#include <stdint.h>

/* Delay before a held button starts repeating, and the repeat period. */
#define REPEAT_DELAY_MS   180u
#define REPEAT_RATE_MS     60u

typedef struct {
    btn_id_t active;        /* button currently under the finger */
    uint32_t held_ms;       /* how long it has been held */
    uint32_t next_fire_ms;  /* when the next repeat should fire */
    bool     paused;
    bool     sound_on;
} input_t;

void input_init(input_t *in);

/* Rotate a raw landscape touch point into portrait canvas coordinates. */
void input_rotate_point(uint16_t raw_x, uint16_t raw_y, int *out_x, int *out_y);

/* Feed one frame of touch state. touched=false means the finger lifted.
 * Applies the resulting actions to g. Returns the button held this frame,
 * or BTN_COUNT when nothing is pressed. */
btn_id_t input_update(input_t *in, tetris_t *g,
                      bool touched, int px, int py, uint32_t dt_ms);

#endif /* INPUT_H_INCLUDED */
