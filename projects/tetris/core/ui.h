/**
 * Portrait game screen laid out like a handheld console: playfield and status
 * on top, a D-pad and face buttons below.
 * All coordinates are in the 480x800 logical portrait space.
 */
#ifndef UI_H_INCLUDED
#define UI_H_INCLUDED

#include "tetris.h"
#include "gfx.h"

/* 24px cells give a 240x480 field, leaving 240px at the bottom for the pad. */
#define CELL      24
#define FIELD_X   16
#define FIELD_Y   64
#define FIELD_W   (TET_W * CELL)          /* 240 */
#define FIELD_H   (TET_H * CELL)          /* 480 */

/* Status panel to the right of the field. */
#define PANEL_X   (FIELD_X + FIELD_W + 14)   /* 270 */
#define PANEL_W   (GFX_W - PANEL_X - 14)     /* 196 */

/* Controls occupy the strip below the field. */
#define PAD_TOP   (FIELD_Y + FIELD_H + 20)   /* 564 */

/* D-pad on the left, face buttons on the right, SNES style. */
#define DPAD_CX   118       /* centre of the d-pad cross */
#define DPAD_CY   650
#define DPAD_ARM  54        /* length of each arm from centre */
#define DPAD_HALF 36        /* half-width of an arm */

/* Pause is a pill centred along the bottom edge; restart sits to its right. */
#define SYS_CY    772       /* centre line of the bottom button row */

#define FACE_CX   362       /* centre of the face-button diamond */
#define FACE_CY   650
#define FACE_R    34        /* button radius */
#define FACE_GAP  58        /* distance from diamond centre to each button */

#define SYS_H     34
#define SYS_W     120
#define SYS_Y     (SYS_CY - SYS_H / 2)

typedef enum {
    BTN_LEFT = 0,
    BTN_RIGHT,
    BTN_SOFT,      /* d-pad down  */
    BTN_HARD,      /* d-pad up    */
    BTN_ROT_CW,    /* A */
    BTN_ROT_CCW,   /* B */
    BTN_HOLD,      /* Y */
    BTN_SOUND,     /* X */
    BTN_PAUSE,     /* START  */
    BTN_RESTART,   /* SELECT */
    BTN_COUNT
} btn_id_t;

typedef enum {
    SHAPE_RECT = 0,
    SHAPE_CIRCLE,
    SHAPE_PILL
} btn_shape_t;

typedef struct {
    int16_t     x, y, w, h;     /* bounding box, used for hit testing */
    btn_shape_t shape;
    uint16_t    tint;           /* face-button colour, 0 for the default */
    const char *label;
} btn_rect_t;

extern const btn_rect_t UI_BUTTONS[BTN_COUNT];

/* Which button contains this portrait-space point, or BTN_COUNT for none. */
btn_id_t ui_hit_test(int x, int y);

/* Draw the whole screen. held_mask has bit n set while button n is pressed. */
void ui_draw(const tetris_t *g, uint32_t held_mask, bool paused, bool sound_on);

/* Paint the parts that never change (background, title, control shells).
 * Call once per framebuffer at startup so ui_draw does not have to clear the
 * whole screen every frame - a full clear is visible as an edge flicker if
 * the panel scans the buffer mid-redraw. */
void ui_draw_static(void);

/* Forget what the side panel last showed, forcing a full repaint of it on the
 * next ui_draw. Call after switching framebuffers for the first time, or any
 * time the buffer contents are not what the cache assumes. */
void ui_invalidate(void);

#endif /* UI_H_INCLUDED */
