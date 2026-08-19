/**
 * Tetris on the STM32H7S78-DK.
 *
 * The panel is 800x480 landscape; the game is laid out 480x800 portrait and
 * gfx.c rotates every draw. Two framebuffers live in external PSRAM and are
 * swapped on the LTDC vertical blank so the picture never tears.
 */
#include "main.h"
#include "stm32h7s78_discovery.h"
#include "stm32h7s78_discovery_lcd.h"
#include "stm32h7s78_discovery_ts.h"

#include "tetris.h"
#include "gfx.h"
#include "ui.h"
#include "input.h"

/* DMA2D 硬體填色，實作在 dma2d.c。 */
void dma2d_init(void);
void dma2d_wait(void);
void dma2d_fill(uint16_t *dst, int px, int py, int pw, int ph, uint16_t color);

/* PSRAM is mapped at XSPI1. The BSP's own layer addresses sit here. */
#define FB0_ADDR   LCD_LAYER_0_ADDRESS   /* 0x90000000 */
#define FB1_ADDR   LCD_LAYER_1_ADDRESS   /* 0x90200000 */

static tetris_t  g_game;
static input_t   g_input;
static uint32_t  g_front;      /* which buffer the LTDC is showing */

/* Frame timing, read over SWD to see whether drawing fits inside a refresh. */
volatile uint32_t g_draw_cycles;
volatile uint32_t g_wait_cycles;
volatile uint32_t g_frames;

/* Enable the cycle counter so frame cost can be measured. */
static void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Mark the framebuffers write-through.
 *
 * The template maps external RAM as cacheable write-back, which is right for
 * general data but wrong for a framebuffer: the CPU's writes sit in D-Cache
 * while the LTDC reads PSRAM directly over its own bus, so the panel scans
 * whatever was there before the cache line is evicted. The visible result is
 * a picture built from a mix of old and new content - the flicker.
 *
 * Write-through keeps reads cached (so read-modify-write drawing stays fast)
 * but sends every write straight to PSRAM, so what the LTDC sees always
 * matches what was drawn. The alternative - cleaning the cache by hand every
 * frame - costs more and is easy to get wrong.
 */
static void framebuffer_mpu_init(void)
{
    MPU_Region_InitTypeDef mpu = {0};

    HAL_MPU_Disable();

    /* One region covering both buffers: 0x90000000, 4MB spans FB0 and FB1. */
    mpu.Enable           = MPU_REGION_ENABLE;
    mpu.Number           = MPU_REGION_NUMBER7;
    mpu.BaseAddress      = FB0_ADDR;
    mpu.Size             = MPU_REGION_SIZE_4MB;
    mpu.AccessPermission = MPU_REGION_FULL_ACCESS;
    mpu.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    mpu.IsCacheable      = MPU_ACCESS_CACHEABLE;
    mpu.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;  /* write-through */
    mpu.TypeExtField     = MPU_TEX_LEVEL0;
    mpu.SubRegionDisable = 0x00;
    mpu.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    HAL_MPU_ConfigRegion(&mpu);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

    /* Drop anything already cached for this range so the first frame is not
     * served from stale write-back lines. */
    SCB_CleanInvalidateDCache();
}

/* Seed the piece bag from something that differs run to run. */
static uint32_t seed_from_hardware(void)
{
    uint32_t s = HAL_GetTick();
    s ^= (uint32_t)(uintptr_t)&g_game;
    s ^= SysTick->VAL << 8;
    return s ? s : 0xA5A5F00Du;
}

static void screen_init(void)
{
    /* BSP_LCD_Init defaults to RGB888, which the LTDC actually programs as
     * ARGB8888 (4 bytes per pixel). The game draws 16-bit pixels, so ask for
     * RGB565 explicitly - otherwise the layer is read at the wrong stride and
     * the picture repeats horizontally with the tail left as PSRAM noise.
     * The BSP only supports landscape; portrait is our own rotation. */
    if (BSP_LCD_InitEx(0, LCD_ORIENTATION_LANDSCAPE, LCD_PIXEL_FORMAT_RGB565,
                       PHYS_W, PHYS_H) != BSP_ERROR_NONE) {
        Error_Handler();
    }

    /* Clear both buffers before anything is shown; PSRAM powers up with
     * random contents and the game only paints part of the screen each frame. */
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        ((uint16_t *)FB0_ADDR)[i] = 0;
        ((uint16_t *)FB1_ADDR)[i] = 0;
    }

    BSP_LCD_SetLayerAddress(0, 0, FB0_ADDR);

    /* Switch the BSP into no-reload mode. With reload enabled,
     * BSP_LCD_SetLayerAddress calls HAL_LTDC_SetAddress, which reloads the
     * layer immediately - mid-frame - and tears. In no-reload mode the
     * address is staged and only takes effect when we ask for a vertical
     * blanking reload in present(). */
    BSP_LCD_Reload(0, BSP_LCD_RELOAD_NONE);

    BSP_LCD_DisplayOn(0);

    g_front = 0;
    gfx_set_framebuffer((uint16_t *)FB1_ADDR);   /* draw into the back buffer */
}

static void touch_init(void)
{
    TS_Init_t init;
    init.Width       = PHYS_W;
    init.Height      = PHYS_H;
    init.Orientation = TS_SWAP_NONE;
    init.Accuracy    = 2;

    if (BSP_TS_Init(0, &init) != BSP_ERROR_NONE) {
        Error_Handler();
    }
}

/* Present the buffer we just drew and start drawing into the other one. */
static void present(void)
{
    /* 交給面板前，確定所有硬體填色都已落地。 */
    dma2d_wait();

    /* g_front is the buffer the LTDC is showing, so we have been drawing into
     * the other one. */
    uint32_t drawn = g_front ? FB0_ADDR : FB1_ADDR;

    /* Write CFBAR directly rather than going through the BSP.
     *
     * BSP_LCD_SetLayerAddress ends up in LTDC_SetConfig, which rewrites the
     * layer's whole register set - window position, pixel format, blending,
     * line length - and several of those are read-modify-write pairs that
     * momentarily hold a cleared value. The panel scans continuously, so any
     * of those gaps can be caught mid-frame and appears as a dark band. That
     * is the real cause of the flicker; which regions get repainted never
     * mattered.
     *
     * CFBAR is shadowed: writing it changes nothing until a reload is
     * requested, and the reload swaps the buffer atomically during vertical
     * blanking. One register write, no intermediate state. */
    LTDC_Layer1->CFBAR = drawn;

    /* Arm the swap for the next vertical blank, then wait for the hardware to
     * take it. The wait is required: until the reload latches, the buffer we
     * are about to start drawing into is still the one being scanned out.
     * Bounded so a stalled panel cannot hang the game. */
    LTDC->SRCR = LTDC_SRCR_VBR;
    uint32_t tw = DWT->CYCCNT;
    for (uint32_t guard = 0; guard < 2000000u; guard++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) {
            break;
        }
    }
    g_wait_cycles = DWT->CYCCNT - tw;
    g_frames++;

    /* The buffer just handed over becomes the front one; draw into the other.
     * Pointing gfx at the buffer being scanned out would tear. */
    g_front ^= 1u;
    gfx_set_framebuffer((uint16_t *)(g_front ? FB0_ADDR : FB1_ADDR));
}

/* Read the panel and convert to portrait coordinates. */
static bool read_touch(int *px, int *py)
{
    TS_State_t st;
    if (BSP_TS_GetState(0, &st) != BSP_ERROR_NONE) {
        return false;
    }
    if (!st.TouchDetected) {
        return false;
    }
    input_rotate_point((uint16_t)st.TouchX, (uint16_t)st.TouchY, px, py);
    return true;
}

void game_run(void)
{
    cycle_counter_init();
    framebuffer_mpu_init();
    screen_init();
    touch_init();

    /* 硬體加速填色。實作在 dma2d.c，並由 dma2d_probe.c 驗證過四種使用情境
     * 皆零錯誤（含大量小矩形、與 CPU 交錯寫入）。 */
    dma2d_init();
    gfx_set_fill_hw(dma2d_fill);

    tetris_init(&g_game, seed_from_hardware());
    input_init(&g_input);

    /* Paint the unchanging parts into both buffers once, so each frame only
     * has to repaint the field and the controls. Restore the draw target
     * afterwards: screen_init left it on the back buffer, and drawing into
     * the buffer the LTDC is showing is exactly what causes flicker. */
    gfx_set_framebuffer((uint16_t *)FB0_ADDR);
    ui_draw_static();
    gfx_set_framebuffer((uint16_t *)FB1_ADDR);
    ui_draw_static();
    gfx_set_framebuffer((uint16_t *)(g_front ? FB0_ADDR : FB1_ADDR));

    uint32_t last = HAL_GetTick();

    for (;;) {
        uint32_t now = HAL_GetTick();
        uint32_t dt  = now - last;
        last = now;
        /* A long stall (debugger break) should not dump the piece instantly. */
        if (dt > 100u) {
            dt = 100u;
        }

        int px = 0, py = 0;
        bool touched = read_touch(&px, &py);

        btn_id_t held = input_update(&g_input, &g_game, touched, px, py, dt);

        if (!g_input.paused) {
            tetris_update(&g_game, dt);
        }

        uint32_t mask = (held < BTN_COUNT) ? (1u << held) : 0u;

        /* Measure how long a frame's drawing takes relative to the panel's
         * refresh period. If drawing outlasts a refresh the swap slips a
         * frame and the picture can be caught part-drawn. */
        uint32_t t0 = DWT->CYCCNT;
        ui_draw(&g_game, mask, g_input.paused, g_input.sound_on);
        g_draw_cycles = DWT->CYCCNT - t0;

        present();

        /* Light the LED while a button is down - a quick sign of life that
         * does not depend on the panel working. */
        if (held < BTN_COUNT) {
            BSP_LED_On(LD1);
        } else {
            BSP_LED_Off(LD1);
        }
    }
}
