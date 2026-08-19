/**
 * DMA2D 矩形填色（不經過 HAL）。
 *
 * 為什麼不用 HAL_DMA2D_Start + HAL_DMA2D_PollForTransfer：
 *
 * PollForTransfer 的等待迴圈包在 `if (CR & DMA2D_CR_START)` 裡面。小矩形
 * 傳輸很快，常常在 Start 返回、程式走到 poll 之前就結束了，此時 START 已被
 * 硬體清 0，整個等待迴圈被跳過。
 *
 * 單獨看沒問題（傳輸確實完成了），但 HAL_DMA2D_Start 從不清除 TC 旗標。於是
 * 下一次傳輸時，上一次殘留的 TC=1 還在，poll 又會立刻返回——這次傳輸卻還在
 * 進行中。接著 CPU 去讀那塊記憶體，或下一次填色蓋上去，就出現「某些格子沒被
 * 這一輪填到，殘留上一輪內容」的現象。
 *
 * 實測（見 dma2d_probe.c）：連續 200 格小矩形，第 2 回合就出現 48 個像素
 * 保留著上一回合的顏色。
 *
 * 正確做法：發起傳輸前先清 TC，等待時直接看 START 位而不是旗標。
 */
#include "main.h"
#include "gfx.h"

/* 低於這個像素數就用 CPU 畫。診斷時設 0 以強制走硬體。 */
uint32_t dma2d_cpu_threshold = 1024u;

/* 統計實際發出的硬體傳輸次數，供診斷用。 */
volatile uint32_t dma2d_start_count;

void dma2d_init(void)
{
    __HAL_RCC_DMA2D_CLK_ENABLE();

    /* R2M：以暫存器中的顏色填滿目的地，不需要來源緩衝區。 */
    MODIFY_REG(DMA2D->CR, DMA2D_CR_MODE, DMA2D_R2M);
    MODIFY_REG(DMA2D->OPFCCR, DMA2D_OPFCCR_CM, DMA2D_OUTPUT_RGB565);

    /* 清掉開機時可能殘留的旗標。 */
    DMA2D->IFCR = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;
}

/* 等到 DMA2D 真的閒置。START 由硬體在傳輸結束時清 0，比旗標可靠。 */
void dma2d_wait(void)
{
    while (DMA2D->CR & DMA2D_CR_START) {
        /* 傳輸進行中 */
    }
    /* 確保 DMA2D 的寫入對後續的 CPU 存取可見。 */
    __DSB();
}

void dma2d_fill(uint16_t *dst, int px, int py, int pw, int ph, uint16_t color)
{
    if (pw <= 0 || ph <= 0) {
        return;
    }

    /* 小矩形用 CPU 反而快：DMA2D 每次都要設定四個暫存器再啟動，
     * 對幾百個像素來說設定成本大於傳輸本身。門檻設 0 可強制全走硬體，
     * 診斷程式用這個方式確保真的在測 DMA2D。 */
    if ((uint32_t)pw * (uint32_t)ph < dma2d_cpu_threshold) {
        /* 走 CPU 之前一定要等硬體停下來。dma2d_fill 是非同步的（發完就返回），
         * 所以前一次的大面積填色可能還在寫。若在那時用 CPU 畫小東西，
         * DMA2D 隨後的寫入會把剛畫好的內容蓋掉。
         *
         * 預覽框正是這個情形：框底 190x68 走硬體，裡面的方塊 14x14 走 CPU。
         * 沒有這個等待，方塊會被框底的填色抹掉，而且每格抹掉的程度不同，
         * 看起來就是「方塊不完整而且會閃」。 */
        dma2d_wait();

        for (int r = 0; r < ph; r++) {
            uint16_t *row = &dst[(uint32_t)(py + r) * PHYS_W + (uint32_t)px];
            for (int c = 0; c < pw; c++) {
                row[c] = color;
            }
        }
        return;
    }

    /* 上一次的傳輸必須真的結束，才能改暫存器。 */
    dma2d_wait();

    /* R2M 模式下 OCOLR 依輸出格式解讀，RGB565 就直接放 16 位元值。
     * （HAL_DMA2D_Start 走的是另一條路：它一律收 ARGB8888 再自己轉換，
     *   這裡直接寫暫存器，所以不需要轉。） */
    DMA2D->OCOLR = color;
    DMA2D->OMAR  = (uint32_t)&dst[(uint32_t)py * PHYS_W + (uint32_t)px];
    DMA2D->OOR   = (uint32_t)(PHYS_W - pw);
    DMA2D->NLR   = ((uint32_t)pw << DMA2D_NLR_PL_Pos) | (uint32_t)ph;

    /* 先清 TC，否則下一次等待會被上一次的旗標騙過去。 */
    DMA2D->IFCR = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;

    DMA2D->CR |= DMA2D_CR_START;
    dma2d_start_count++;
}
