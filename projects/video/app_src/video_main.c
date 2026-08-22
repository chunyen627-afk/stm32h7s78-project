/**
 * MJPEG 影片播放：先量真實格率，再談功能。
 *
 * 這個專案的存在理由是相簿那邊留下的一個未解問題：JPEG 硬體用 DMA 解碼在
 * **隔離測試裡完全正確**（輸出與輪詢逐位元組相同、連續 40 次穩定、快 4.8 倍），
 * 但整合進相簿的正式路徑就會在第一張之後把週邊卡死，連 HAL_JPEG_Abort()
 * 都救不回來（board-notes 16.6）。唯一的差別是「兩次解碼之間夾了什麼」——
 * 隔離測試背對背連續呼叫，相簿中間隔著 SD 讀檔、轉色、縮放、顯示與等待。
 *
 * 影片正好是那個問題的極端版本：每秒要連續解二十幾次，而且中間夾著轉色與
 * 換頁。在一個乾淨的環境裡處理它，比在相簿裡邊改邊壞好得多。
 *
 * 刻意砍掉的東西：沒有 SD（影格燒在外部 Flash 的空白區，記憶體映射直接讀）、
 * 沒有觸控、沒有選單、沒有中文字型、沒有縮放（影格已經是面板尺寸）。
 * 留下的只有「解碼 -> 轉色 -> 顯示」這條最短路徑，這樣量到的數字才乾淨。
 */
#include "main.h"
#include "stm32h7s78_discovery_lcd.h"
#include "jpeg_utils.h"

#include <stdbool.h>
#include <string.h>

/* 影格包燒在外部 Flash 的空白區。app 只用到約 1.5MB，128MB 綽綽有餘。
 * 外部 Flash 是記憶體映射的，所以這裡直接就地讀，不必複製到 RAM。 */
#define FRAMES_BASE     ((const uint8_t *)0x71000000u)
#define FRAMES_MAGIC    0x4D524656u          /* 'VFRM' 小端序 */

/* 面板實體尺寸。影片專案不畫任何 UI，所以不引入繪圖層 —— 它會拉進
 * 中文字型表，對一個只顯示影格的韌體是純粹的負擔。 */
#define PHYS_W          800
#define PHYS_H          480

#define FB0_ADDR        LCD_LAYER_0_ADDRESS  /* 0x90000000 */
#define FB1_ADDR        LCD_LAYER_1_ADDRESS  /* 0x90200000 */

/* PSRAM：YCbCr 中間緩衝區。800x480 的 4:2:0 只要 576KB，配 2MB 有餘裕。 */
#define YCBCR_BUF       ((uint8_t *)0x90600000u)
#define YCBCR_CAP       (2u * 1024u * 1024u)

/* HPDMA 的區塊大小欄位只有 16 位元，而 HAL 是 (size & 0xFFFF) 遮掉、不檢查。
 * 一次交出整塊會被默默截斷或直接變成 0，所以必須分塊餵（board-notes 16.2）。 */
#define CHUNK           (32u * 1024u)

typedef struct {
    uint32_t magic, count, width, height;
} frames_hdr_t;

static JPEG_HandleTypeDef g_hjpeg;
static DMA_HandleTypeDef  g_dma_in;
static DMA_HandleTypeDef  g_dma_out;
static uint32_t           g_front;

static volatile uint8_t   g_done;        /* 0=進行中 1=完成 2=錯誤 */
static volatile uint32_t  g_out_total;
static const uint8_t     *g_in_ptr;
static uint32_t           g_in_left;

/* 診斷用，SWD 讀得到。板子沒有 UART，狀態一律走這裡。 */
volatile uint32_t g_dbg_stage;
volatile uint32_t g_dbg_nframe;          /* 已顯示的影格數 */
volatile uint32_t g_dbg_fail;            /* 解碼失敗次數 */
volatile int32_t  g_dbg_lasterr;
volatile uint32_t g_dbg_jerr, g_dbg_derr_in, g_dbg_derr_out;
volatile uint32_t g_dbg_hdr_magic, g_dbg_hdr_count, g_dbg_hdr_w, g_dbg_hdr_h;

/* 每一段的累計微秒與影格數，算平均用。單格數字會跳，看平均才準。 */
volatile uint32_t g_dbg_sum_dec, g_dbg_sum_cc, g_dbg_sum_out, g_dbg_sum_all;
volatile uint32_t g_dbg_fps_x100;        /* 實測格率 x100 */

static inline uint32_t cyc_start(void) { return DWT->CYCCNT; }
static inline uint32_t cyc_us(uint32_t t0)
{
    return (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u);
}

/* ------------------------------------------------------------------ */
/* JPEG 回呼：分塊接續                                                  */
/* ------------------------------------------------------------------ */

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataOut,
                                uint32_t OutDataLength)
{
    uint32_t end = (uint32_t)(pDataOut - YCBCR_BUF) + OutDataLength;
    uint32_t left;

    if (end > g_out_total) {
        g_out_total = end;      /* 取最大值不累加：HAL 可能對同一批回呼兩次 */
    }
    left = (end < YCBCR_CAP) ? (YCBCR_CAP - end) : 0u;
    if (left > CHUNK) { left = CHUNK; }
    if (left >= 4u) {
        (void)HAL_JPEG_ConfigOutputBuffer(hjpeg, YCBCR_BUF + end, left);
    }
}

void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *hjpeg, uint32_t NbDecodedData)
{
    uint32_t n;

    if (NbDecodedData >= g_in_left) {
        g_in_left = 0;
        return;
    }
    g_in_ptr  += NbDecodedData;
    g_in_left -= NbDecodedData;
    n = (g_in_left > CHUNK) ? CHUNK : g_in_left;
    HAL_JPEG_ConfigInputBuffer(hjpeg, (uint8_t *)g_in_ptr, n);
}

void HAL_JPEG_DecodeCpltCallback(JPEG_HandleTypeDef *hjpeg) { (void)hjpeg; g_done = 1u; }
void HAL_JPEG_ErrorCallback(JPEG_HandleTypeDef *hjpeg)      { (void)hjpeg; g_done = 2u; }

void HPDMA1_Channel0_IRQHandler(void) { HAL_DMA_IRQHandler(&g_dma_in); }
void HPDMA1_Channel1_IRQHandler(void) { HAL_DMA_IRQHandler(&g_dma_out); }
void JPEG_IRQHandler(void)            { HAL_JPEG_IRQHandler(&g_hjpeg); }

void HAL_JPEG_MspInit(JPEG_HandleTypeDef *hjpeg)
{
    if (hjpeg->Instance != JPEG) {
        return;
    }
    __HAL_RCC_JPEG_CLK_ENABLE();
    /* 這行 ST 的範例放在獨立的 MX_HPDMA1_Init()，很容易漏 —— 漏了的話
     * HAL_DMA_Init() 照樣回傳 HAL_OK，但傳輸永遠不會開始。 */
    __HAL_RCC_HPDMA1_CLK_ENABLE();

    g_dma_out.Instance                  = HPDMA1_Channel1;
    g_dma_out.Init.Request              = HPDMA1_REQUEST_JPEG_TX;
    g_dma_out.Init.BlkHWRequest         = DMA_BREQ_SINGLE_BURST;
    g_dma_out.Init.Direction            = DMA_PERIPH_TO_MEMORY;
    g_dma_out.Init.SrcInc               = DMA_SINC_FIXED;
    g_dma_out.Init.DestInc              = DMA_DINC_INCREMENTED;
    g_dma_out.Init.SrcDataWidth         = DMA_SRC_DATAWIDTH_WORD;
    g_dma_out.Init.DestDataWidth        = DMA_DEST_DATAWIDTH_WORD;
    g_dma_out.Init.Priority             = DMA_LOW_PRIORITY_LOW_WEIGHT;
    g_dma_out.Init.SrcBurstLength       = 8;
    g_dma_out.Init.DestBurstLength      = 8;
    g_dma_out.Init.TransferAllocatedPort =
        DMA_SRC_ALLOCATED_PORT1 | DMA_DEST_ALLOCATED_PORT0;
    g_dma_out.Init.TransferEventMode    = DMA_TCEM_BLOCK_TRANSFER;
    g_dma_out.Init.Mode                 = DMA_NORMAL;
    (void)HAL_DMA_Init(&g_dma_out);
    __HAL_LINKDMA(hjpeg, hdmaout, g_dma_out);

    g_dma_in.Instance                   = HPDMA1_Channel0;
    g_dma_in.Init.Request               = HPDMA1_REQUEST_JPEG_RX;
    g_dma_in.Init.BlkHWRequest          = DMA_BREQ_SINGLE_BURST;
    g_dma_in.Init.Direction             = DMA_MEMORY_TO_PERIPH;
    g_dma_in.Init.SrcInc                = DMA_SINC_INCREMENTED;
    g_dma_in.Init.DestInc               = DMA_DINC_FIXED;
    g_dma_in.Init.SrcDataWidth          = DMA_SRC_DATAWIDTH_WORD;
    g_dma_in.Init.DestDataWidth         = DMA_DEST_DATAWIDTH_WORD;
    g_dma_in.Init.Priority              = DMA_LOW_PRIORITY_LOW_WEIGHT;
    g_dma_in.Init.SrcBurstLength        = 8;
    g_dma_in.Init.DestBurstLength       = 8;
    g_dma_in.Init.TransferAllocatedPort =
        DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
    g_dma_in.Init.TransferEventMode     = DMA_TCEM_BLOCK_TRANSFER;
    g_dma_in.Init.Mode                  = DMA_NORMAL;
    (void)HAL_DMA_Init(&g_dma_in);
    __HAL_LINKDMA(hjpeg, hdmain, g_dma_in);

    HAL_NVIC_SetPriority(HPDMA1_Channel0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(HPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(HPDMA1_Channel1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(HPDMA1_Channel1_IRQn);
    HAL_NVIC_SetPriority(JPEG_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(JPEG_IRQn);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

static void psram_mpu_init(void)
{
    MPU_Region_InitTypeDef mpu = {0};

    /* framebuffer 與 YCbCr 都被硬體讀寫（LTDC / JPEG DMA），維持 write-through
     * 才不會讓 CPU 的寫入卡在快取裡（board-notes 2.4）。 */
    HAL_MPU_Disable();
    mpu.Enable           = MPU_REGION_ENABLE;
    mpu.Number           = MPU_REGION_NUMBER7;
    mpu.BaseAddress      = 0x90000000u;
    mpu.Size             = MPU_REGION_SIZE_32MB;
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
    /* BSP 預設 ARGB8888，不指定 RGB565 的話會用錯的列距讀，畫面橫向重複。 */
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
    /* DisplayOn 不會開背光，DisplayOff 卻會關 —— 要自己把 GPIOG15 拉高
     * （board-notes 12.1）。 */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_15, GPIO_PIN_SET);
    g_front = 0;
}

static void present(void)
{
    /* 只寫 CFBAR：它是 shadow register，等垂直消隱重載時整批原子切換。
     * 用 BSP_LCD_SetLayerAddress 會重寫整組圖層暫存器而撕裂（board-notes 2.2）。 */
    LTDC_Layer1->CFBAR = g_front ? FB0_ADDR : FB1_ADDR;
    LTDC->SRCR = LTDC_SRCR_VBR;
    for (uint32_t g = 0; g < 2000000u; g++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) { break; }
    }
    g_front ^= 1u;
}

/* ------------------------------------------------------------------ */
/* 播放                                                                */
/* ------------------------------------------------------------------ */

static bool decode_frame(const uint8_t *jpg, uint32_t size)
{
    g_out_total = 0;
    g_in_ptr    = jpg;
    g_in_left   = size;
    g_done      = 0u;

    if (HAL_JPEG_Decode_DMA(&g_hjpeg, (uint8_t *)jpg,
                            (size > CHUNK) ? CHUNK : size,
                            YCBCR_BUF, CHUNK) != HAL_OK) {
        g_dbg_lasterr = -3;
        return false;
    }
    {
        uint32_t t0 = HAL_GetTick();
        while (g_done == 0u) {
            if ((HAL_GetTick() - t0) > 2000u) {
                g_dbg_lasterr = -4;
                (void)HAL_JPEG_Abort(&g_hjpeg);
                return false;
            }
        }
    }
    if (g_done != 1u) {
        g_dbg_lasterr   = -5;
        g_dbg_jerr      = HAL_JPEG_GetError(&g_hjpeg);
        g_dbg_derr_in   = HAL_DMA_GetError(&g_dma_in);
        g_dbg_derr_out  = HAL_DMA_GetError(&g_dma_out);
        /* 啟動了非同步硬體的失敗路徑一定要收乾淨，否則週邊停在忙碌狀態，
         * 之後每次解碼都拿到 HAL_BUSY（board-notes 16.3）。 */
        (void)HAL_JPEG_Abort(&g_hjpeg);
        return false;
    }
    return true;
}

void video_run(void)
{
    const frames_hdr_t *hdr = (const frames_hdr_t *)FRAMES_BASE;
    const uint32_t     *tbl = (const uint32_t *)(FRAMES_BASE + sizeof(*hdr));
    JPEG_ConfTypeDef    info;
    JPEG_YCbCrToRGB_Convert_Function convert;
    uint32_t nb_mcu = 0, converted = 0;
    uint32_t idx = 0, t_run;

    g_dbg_stage = 1;
    psram_mpu_init();
    screen_init();

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    g_hjpeg.Instance = JPEG;
    if (HAL_JPEG_Init(&g_hjpeg) != HAL_OK) {
        g_dbg_stage = 90;
        for (;;) { }
    }
    JPEG_InitColorTables();

    g_dbg_hdr_magic = hdr->magic;
    g_dbg_hdr_count = hdr->count;
    g_dbg_hdr_w     = hdr->width;
    g_dbg_hdr_h     = hdr->height;
    if (hdr->magic != FRAMES_MAGIC || hdr->count == 0u) {
        g_dbg_stage = 91;               /* 影格包沒燒進去或位址不對 */
        for (;;) { }
    }

    g_dbg_stage = 5;
    t_run = HAL_GetTick();

    for (;;) {
        const uint8_t *jpg = FRAMES_BASE + tbl[idx * 2u];
        uint32_t       len = tbl[idx * 2u + 1u];
        uint32_t       c_all = cyc_start();
        uint32_t       c0;

        c0 = cyc_start();
        if (!decode_frame(jpg, len)) {
            g_dbg_fail++;
            idx = (idx + 1u) % hdr->count;
            continue;
        }
        g_dbg_sum_dec += cyc_us(c0);

        if (HAL_JPEG_GetInfo(&g_hjpeg, &info) != HAL_OK ||
            JPEG_GetDecodeColorConvertFunc(&info, &convert, &nb_mcu) != HAL_OK) {
            g_dbg_fail++;
            idx = (idx + 1u) % hdr->count;
            continue;
        }

        /* JPEG 的 DMA 繞過 D-Cache 直接寫 PSRAM，CPU 讀之前要先失效，
         * 否則讀到上一格的殘留（board-notes 3.1）。 */
        SCB_InvalidateDCache_by_Addr((uint32_t *)YCBCR_BUF, (int32_t)g_out_total);

        /* 影格已經是面板尺寸，不需要縮放也不需要旋轉 —— 轉色直接寫進
         * 背景 framebuffer。這是整條路徑最短的形式。 */
        c0 = cyc_start();
        (void)convert(YCBCR_BUF,
                      (uint8_t *)(g_front ? FB1_ADDR : FB0_ADDR),
                      0, g_out_total, &converted);
        g_dbg_sum_cc += cyc_us(c0);

        c0 = cyc_start();
        present();
        g_dbg_sum_out += cyc_us(c0);

        g_dbg_sum_all += cyc_us(c_all);
        g_dbg_nframe++;

        /* 每 60 格更新一次實測格率，用牆鐘時間算才包含所有開銷。 */
        if ((g_dbg_nframe % 60u) == 0u) {
            uint32_t ms = HAL_GetTick() - t_run;

            if (ms > 0u) {
                g_dbg_fps_x100 = (g_dbg_nframe * 100000u) / ms;
            }
        }
        idx = (idx + 1u) % hdr->count;
    }
}
