/**
 * 賽車儀表：SimHub 經 ST-LINK 的虛擬 COM 埠（UART4）送遙測，畫在 7 吋螢幕上。
 *
 * 資料格式是 DSH1（沿用 caa1211/esp32-gt7-dashboard，MIT），SimHub 那邊用
 * Custom Serial devices 外掛、公式在 projects/racedash/simhub/。
 *
 * 螢幕初始化、雙緩衝、垂直消隱換頁照搬 projects/tetris 驗證過的做法
 * （為什麼這樣寫的說明在那邊的 game_main.c），差別是這裡用橫向 800x480。
 */
#include "main.h"
#include "stm32h7s78_discovery.h"
#include "stm32h7s78_discovery_lcd.h"

#include "gfx.h"
#include "dsh1.h"
#include "dash.h"
#include "serial.h"

#include <string.h>

void dma2d_init(void);
void dma2d_wait(void);
void dma2d_fill(uint16_t *dst, int px, int py, int pw, int ph, uint16_t color);

#define FB0_ADDR   LCD_LAYER_0_ADDRESS   /* 0x90000000，PSRAM */
#define FB1_ADDR   LCD_LAYER_1_ADDRESS   /* 0x90200000 */

#define SERIAL_BAUD   115200u
#define STALE_MS      2000u     /* 這麼久沒收到有效的一行就回等待畫面 */

/* 狀態放 DTCM，重置不會洗掉，用 SWD 讀得到（相簿黑盒子同一個位置，
 * 兩個專案不會同時在板子上）。[0] 魔術字 "RDSH"、[1] tick、[2] 收到的位元組、
 * [3] 有效行、[4] 無效行、[5] 畫了幾格、[6] 最後的序號、[7] 一格畫多少週期、
 * [8] 序列埠溢位、[9] 太長被丟的行 */
#define BOX ((volatile uint32_t *)0x20004020u)

static uint32_t g_front;

static void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* framebuffer 設成 write-through：CPU 寫的東西要立刻落到 PSRAM，
 * LTDC 繞過快取直接讀（tetris 的 game_main.c 有完整說明）。 */
static void framebuffer_mpu_init(void)
{
    MPU_Region_InitTypeDef mpu = {0};

    HAL_MPU_Disable();
    mpu.Enable           = MPU_REGION_ENABLE;
    mpu.Number           = MPU_REGION_NUMBER7;
    mpu.BaseAddress      = FB0_ADDR;
    mpu.Size             = MPU_REGION_SIZE_4MB;
    mpu.AccessPermission = MPU_REGION_FULL_ACCESS;
    mpu.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    mpu.IsCacheable      = MPU_ACCESS_CACHEABLE;
    mpu.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    mpu.TypeExtField     = MPU_TEX_LEVEL0;
    mpu.SubRegionDisable = 0x00;
    mpu.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    HAL_MPU_ConfigRegion(&mpu);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
    SCB_CleanInvalidateDCache();
}

static void screen_init(void)
{
    if (BSP_LCD_InitEx(0, LCD_ORIENTATION_LANDSCAPE, LCD_PIXEL_FORMAT_RGB565,
                       PHYS_W, PHYS_H) != BSP_ERROR_NONE) {
        Error_Handler();
    }
    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        ((uint16_t *)FB0_ADDR)[i] = 0;
        ((uint16_t *)FB1_ADDR)[i] = 0;
    }
    BSP_LCD_SetLayerAddress(0, 0, FB0_ADDR);
    BSP_LCD_Reload(0, BSP_LCD_RELOAD_NONE);
    BSP_LCD_DisplayOn(0);

    g_front = 0;
    gfx_set_orientation(true);                   /* 儀表是橫的：800x480 */
    gfx_set_framebuffer((uint16_t *)FB1_ADDR);
}

static void present(void)
{
    dma2d_wait();
    LTDC_Layer1->CFBAR = g_front ? FB0_ADDR : FB1_ADDR;
    LTDC->SRCR = LTDC_SRCR_VBR;
    for (uint32_t guard = 0; guard < 2000000u; guard++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) { break; }
    }
    g_front ^= 1u;
    gfx_set_framebuffer((uint16_t *)(g_front ? FB0_ADDR : FB1_ADDR));
}

void racedash_run(void)
{
    static char   line[512];
    static char   work[512];
    static dsh1_t tel;
    dash_link_t   link = {0};
    uint32_t      last_ok = 0;
    bool          have = false;

    for (int i = 0; i < 16; i++) { BOX[i] = 0u; }
    BOX[0] = 0x52445348u;                        /* "RDSH" */

    cycle_counter_init();
    framebuffer_mpu_init();
    screen_init();
    dma2d_init();
    gfx_set_fill_hw(dma2d_fill);
    gfx_set_sync_hw(dma2d_wait);

    if (!serial_init(SERIAL_BAUD)) {
        Error_Handler();
    }

    for (;;) {
        uint32_t now = HAL_GetTick();
        int      n;

        /* 截圖用：SWD 寫 BOX[15] = "FROZ" 就停止重畫。從板子讀一整張畫面要
         * 將近一秒，期間儀表會重畫幾十次（每次先清黑再畫），讀到的是很多格
         * 混在一起的橫紋。凍住之後兩塊 framebuffer 都是完整的一格。 */
        if (BOX[15] == 0x5A4F5246u) {
            BOX[1] = now;
            continue;
        }

        while ((n = serial_read_line(line, sizeof(line))) >= 0) {
            if (n == 0) { continue; }
            memcpy(work, line, (size_t)n + 1u);  /* 解析會切開字串，留原文好除錯 */
            if (dsh1_parse(work, &tel)) {
                link.lines_ok++;
                last_ok = now;
                have = true;
                BOX[6] = tel.seq;
            } else {
                link.lines_bad++;
            }
        }
        link.rx_bytes = g_serial_rx_bytes;

        {
            uint32_t t0 = DWT->CYCCNT;

            if (have && tel.running && (now - last_ok) < STALE_MS) {
                dash_draw(&tel, now);
            } else {
                dash_draw_waiting(&link, now);
            }
            BOX[7] = DWT->CYCCNT - t0;
        }
        present();

        BOX[1] = now;
        BOX[2] = link.rx_bytes;
        BOX[3] = link.lines_ok;
        BOX[4] = link.lines_bad;
        BOX[5]++;
        BOX[8] = g_serial_rx_overrun;
        BOX[9] = g_serial_line_dropped;
    }
}
