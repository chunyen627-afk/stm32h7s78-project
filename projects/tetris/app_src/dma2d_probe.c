/**
 * DMA2D 診斷程式。
 *
 * 遊戲裡的雜點是偶發的，很難在正常流程中定位。這支程式把 DMA2D 單獨拉出來，
 * 用固定樣式反覆填色再逐一驗證內容，把「偶發」變成「可量測的錯誤率」。
 *
 * 結果寫進全域變數，用 SWD 讀出來，不需要 UART。
 */
#include "main.h"
#include "stm32h7s78_discovery.h"
#include "stm32h7s78_discovery_lcd.h"
#include "gfx.h"

void dma2d_init(void);
void dma2d_wait(void);
void dma2d_fill(uint16_t *dst, int px, int py, int pw, int ph, uint16_t color);
extern uint32_t dma2d_cpu_threshold;

#define FB0_ADDR   LCD_LAYER_0_ADDRESS
#define FB1_ADDR   LCD_LAYER_1_ADDRESS

/* 診斷結果，用 STM32_Programmer_CLI -r32 讀取。 */
volatile uint32_t probe_stage;        /* 目前跑到哪一階段 */
volatile uint32_t probe_iterations;   /* 已完成的回合數 */
volatile uint32_t probe_errors;       /* 累計錯誤像素數 */
volatile uint32_t probe_first_bad;    /* 第一個錯誤的位移 */
volatile uint32_t probe_expect;       /* 期望值 */
volatile uint32_t probe_actual;       /* 實際值 */
volatile uint32_t probe_cycles;       /* 該階段耗時 */
volatile uint32_t probe_starts;       /* 實際發出的傳輸次數 */
volatile uint32_t probe_isr;          /* 失敗當下的 DMA2D ISR */
volatile uint32_t probe_cr;           /* 失敗當下的 DMA2D CR */
volatile uint32_t probe_nlr;          /* 失敗當下的 NLR */
volatile uint32_t probe_omar;         /* 失敗當下的 OMAR */
volatile uint32_t probe_err_dma;      /* DMA2D 那半的錯誤 */
volatile uint32_t probe_err_cpu;      /* CPU 那半的錯誤 */

/* 用受測的實作填色，並等它真的結束（驗證前必須確定寫完）。 */
static void fill(uint16_t *dst, int px, int py, int pw, int ph, uint16_t c)
{
    dma2d_fill(dst, px, py, pw, ph, c);
    dma2d_wait();
}

/* 檢查一塊矩形是否全是預期的顏色。回傳錯誤像素數。 */
static uint32_t verify(const uint16_t *dst, int px, int py,
                       int pw, int ph, uint16_t expect)
{
    uint32_t bad = 0;

    /* DMA2D 直接寫 PSRAM，繞過 CPU 的 D-Cache。CPU 這時若從快取讀，看到的
     * 會是 DMA2D 寫入之前的舊內容。讀取前必須讓這塊區域的快取失效。
     * write-through 只保證「寫入」穿透，管不到「讀取」這個方向。 */
    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *)&dst[(uint32_t)py * PHYS_W + (uint32_t)px],
        (int32_t)((uint32_t)ph * PHYS_W * sizeof(uint16_t)));
    for (int r = 0; r < ph; r++) {
        const uint16_t *row = &dst[(uint32_t)(py + r) * PHYS_W + (uint32_t)px];
        for (int c = 0; c < pw; c++) {
            if (row[c] != expect) {
                if (bad == 0) {
                    probe_first_bad = (uint32_t)((py + r) * PHYS_W + px + c);
                    probe_expect    = expect;
                    probe_actual    = row[c];
                    probe_isr       = DMA2D->ISR;
                    probe_cr        = DMA2D->CR;
                    probe_nlr       = DMA2D->NLR;
                    probe_omar      = DMA2D->OMAR;
                }
                bad++;
            }
        }
    }
    return bad;
}

/* 階段一：單次大面積填色。最簡單的情況，這都錯就是基本設定有問題。 */
static void stage_single_large(uint16_t *fb)
{
    probe_stage = 1;
    probe_errors = 0;
    for (uint32_t i = 0; i < 200; i++) {
        uint16_t c = (uint16_t)(0x0821u * (i + 1));
        fill(fb, 0, 0, PHYS_W, PHYS_H, c);
        probe_errors += verify(fb, 0, 0, PHYS_W, PHYS_H, c);
        probe_iterations = i + 1;
        if (probe_errors) {
            return;
        }
    }
}

/* 階段二：大量小矩形，模擬遊戲每格 200+ 次填色的用法。 */
static void stage_many_small(uint16_t *fb)
{
    probe_stage = 2;
    probe_errors = 0;
    probe_iterations = 0;

    for (uint32_t frame = 0; frame < 100; frame++) {
        uint16_t c = (uint16_t)(0x1084u * (frame + 1));
        /* 24x24 的格子，跟遊戲場地一樣的尺寸與數量。 */
        for (int gy = 0; gy < 20; gy++) {
            for (int gx = 0; gx < 10; gx++) {
                fill(fb, 100 + gx * 24, 50 + gy * 24, 24, 24, c);
            }
        }
        for (int gy = 0; gy < 20; gy++) {
            for (int gx = 0; gx < 10; gx++) {
                probe_errors += verify(fb, 100 + gx * 24, 50 + gy * 24,
                                       24, 24, c);
            }
        }
        probe_iterations = frame + 1;
        if (probe_errors) {
            return;
        }
    }
}

/* 階段三：DMA2D 填色後立刻用 CPU 寫入相鄰區域，測試兩者的交互影響。 */
static void stage_mixed_cpu(uint16_t *fb)
{
    probe_stage = 3;
    probe_errors = 0;
    probe_iterations = 0;

    for (uint32_t i = 0; i < 200; i++) {
        uint16_t cd = (uint16_t)(0x2108u * (i + 1));
        uint16_t cc = (uint16_t)(0x4210u * (i + 1));

        /* DMA2D 畫左半，CPU 立刻畫右半。 */
        fill(fb, 0, 0, 400, 200, cd);
        for (int r = 0; r < 200; r++) {
            uint16_t *row = &fb[(uint32_t)r * PHYS_W + 400];
            for (int c = 0; c < 400; c++) {
                row[c] = cc;
            }
        }

        /* CPU 的寫入也要落地才能驗證。 */
        __DSB();
        uint32_t e_dma = verify(fb, 0, 0, 400, 200, cd);
        uint32_t e_cpu = verify(fb, 400, 0, 400, 200, cc);
        probe_err_dma += e_dma;
        probe_err_cpu += e_cpu;
        probe_errors += e_dma + e_cpu;
        probe_iterations = i + 1;
        if (probe_errors) {
            return;
        }
    }
}

/* 階段四：不等待完成就發下一個傳輸，看硬體是否會自己排隊。 */
static void stage_no_wait(uint16_t *fb)
{
    probe_stage = 4;
    probe_errors = 0;
    probe_iterations = 0;

    for (uint32_t i = 0; i < 100; i++) {
        uint16_t c = (uint16_t)(0x0421u * (i + 1));
        for (int gy = 0; gy < 20; gy++) {
            for (int gx = 0; gx < 10; gx++) {
                /* 只在前一次真的結束後才發，但不用 HAL 的 poll。 */
                /* 不逐次等待，讓 dma2d_fill 自己處理同步。 */
                dma2d_fill(fb, 100 + gx * 24, 50 + gy * 24, 24, 24, c);
            }
        }
        dma2d_wait();

        for (int gy = 0; gy < 20; gy++) {
            for (int gx = 0; gx < 10; gx++) {
                probe_errors += verify(fb, 100 + gx * 24, 50 + gy * 24,
                                       24, 24, c);
            }
        }
        probe_iterations = i + 1;
        if (probe_errors) {
            return;
        }
    }
}

void dma2d_probe_run(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    dma2d_init();
    /* 強制每次填色都走硬體，否則小矩形會被 CPU 路徑接走，測不到 DMA2D。 */
    dma2d_cpu_threshold = 0;

    /* 用背景緩衝區測試，前景保持顯示以便看出程式還活著。 */
    uint16_t *fb = (uint16_t *)FB1_ADDR;

    uint32_t t0 = DWT->CYCCNT;
    stage_single_large(fb);
    probe_cycles = DWT->CYCCNT - t0;
    if (probe_errors) { for (;;) { BSP_LED_Toggle(LD1); HAL_Delay(100); } }

    t0 = DWT->CYCCNT;
    stage_many_small(fb);
    probe_cycles = DWT->CYCCNT - t0;
    if (probe_errors) { for (;;) { BSP_LED_Toggle(LD1); HAL_Delay(200); } }

    t0 = DWT->CYCCNT;
    stage_mixed_cpu(fb);
    probe_cycles = DWT->CYCCNT - t0;
    if (probe_errors) { for (;;) { BSP_LED_Toggle(LD1); HAL_Delay(400); } }

    t0 = DWT->CYCCNT;
    stage_no_wait(fb);
    probe_cycles = DWT->CYCCNT - t0;

    /* 全部通過：LED 恆亮。 */
    probe_stage = 99;
    BSP_LED_On(LD1);
    for (;;) { }
}
