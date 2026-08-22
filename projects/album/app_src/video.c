/**
 * MJPEG 影片播放的解碼引擎（相簿版）。
 *
 * 從 projects/video 那個獨立專案搬過來，只保留「解一格、轉色、寫進
 * framebuffer」這條最短路徑。播放迴圈與 UI 留在 album_main.c。
 *
 * 這裡的每一段幾乎都對應 board-notes 第十六章的一條教訓，改動前先讀那一章。
 */
#include "main.h"
#include "video.h"
#include "photo.h"
#include "jpeg_utils.h"
#include "ff.h"

#include <string.h>

/* PSRAM 借用照片路徑的 YCbCr 區（PSRAM+0x600000 起 10MB）。
 *
 * 照片與影片不會同時跑，所以這是純借用、不需要新配置 —— 相簿的 32MB
 * 早就排滿了（見 photo.c 的配置表），再切新區域只會擠到別人。
 *
 *   +0x00600000  YCbCr 解碼輸出   1MB   800x480 4:2:0 實際只要 576KB
 *   +0x00700000  單格 JPEG        1MB   實測每格 18KB、最大 27KB
 *   +0x00800000  位移表           2MB   夠 262144 格（約三小時）
 */
#define PSRAM_BASE      0x90000000u
#define VID_YCBCR       ((uint8_t *)(PSRAM_BASE + 0x00600000u))
#define VID_YCBCR_CAP   (1u * 1024u * 1024u)
#define VID_FRAME       ((uint8_t *)(PSRAM_BASE + 0x00700000u))
#define VID_FRAME_CAP   (1u * 1024u * 1024u)
#define VID_TBL         ((uint32_t *)(PSRAM_BASE + 0x00800000u))
#define VID_TBL_CAP     (2u * 1024u * 1024u)

#define VIDEO_MAGIC     0x32524656u          /* 'VFR2' 小端序 */
#define VIDEO_HDR_BYTES 24u

#define PHYS_W          800u
#define PHYS_H          480u

/* HPDMA 的區塊大小欄位只有 16 位元，而 HAL 是 (size & 0xFFFF) 遮掉、不檢查，
 * 一次交出整塊會被默默截斷。必須分塊餵（board-notes 16.2）。 */
#define CHUNK           (32u * 1024u)

typedef struct {
    uint32_t magic, count, width, height, fps_x100, max_size;
} hdr_t;

static JPEG_HandleTypeDef  g_hjpeg;
static DMA2D_HandleTypeDef g_hdma2d;
static DMA_HandleTypeDef   g_dma_in;
static DMA_HandleTypeDef   g_dma_out;

static FIL       g_fil;
static bool      g_open;
static bool      g_active;           /* 回呼要不要轉交到這裡 */
static bool      g_dma2d_ready;
static uint32_t  g_count;

static volatile uint8_t  g_done;     /* 0=進行中 1=完成 2=錯誤 */
static volatile uint32_t g_out_total;
static const uint8_t    *g_in_ptr;
static uint32_t          g_in_left;

volatile uint32_t g_vdbg_decoded;
volatile uint32_t g_vdbg_fail;
volatile int32_t  g_vdbg_lasterr;
volatile uint32_t g_vdbg_us_read;
volatile uint32_t g_vdbg_us_dec;
volatile uint32_t g_vdbg_us_cc;

static inline uint32_t cyc_start(void) { return DWT->CYCCNT; }
static inline uint32_t cyc_us(uint32_t t0)
{
    return (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u);
}

/* ------------------------------------------------------------------ */
/* JPEG 回呼（由 photo.c 轉交過來）                                     */
/* ------------------------------------------------------------------ */

bool video_jpeg_active(void) { return g_active; }

void video_jpeg_data_ready(void *hjpeg, uint8_t *pDataOut, uint32_t len)
{
    uint32_t end = (uint32_t)(pDataOut - VID_YCBCR) + len;
    uint32_t left;

    /* 取最大值不累加：HAL 對同一批資料可能回呼兩次（board-notes 11.3）。 */
    if (end > g_out_total) {
        g_out_total = end;
    }
    left = (end < VID_YCBCR_CAP) ? (VID_YCBCR_CAP - end) : 0u;
    if (left > CHUNK) {
        left = CHUNK;
    }
    /* 分塊 DMA 模式下在回呼裡接續餵輸出緩衝區是**本來就該做的事** ——
     * 11.3 那條「不要在回呼裡 ConfigOutputBuffer」針對的是輪詢模式
     * 一開始就把整塊交出去的用法（board-notes 16.2 的註）。 */
    if (left >= 4u) {
        (void)HAL_JPEG_ConfigOutputBuffer((JPEG_HandleTypeDef *)hjpeg,
                                          VID_YCBCR + end, left);
    }
}

void video_jpeg_get_data(void *hjpeg, uint32_t nb_decoded)
{
    uint32_t n;

    if (nb_decoded >= g_in_left) {
        g_in_left = 0;
        /* 沒有資料了要**明講**。直接 return 的話 HAL 會沿用上一次的指標與
         * 長度再送一次同一塊（JPEG_DMAInCpltCallback 只檢查長度 > 0）。 */
        HAL_JPEG_ConfigInputBuffer((JPEG_HandleTypeDef *)hjpeg,
                                   (uint8_t *)g_in_ptr, 0u);
        return;
    }
    g_in_ptr  += nb_decoded;
    g_in_left -= nb_decoded;
    n = (g_in_left > CHUNK) ? CHUNK : g_in_left;

    /* 長度一定要是 4 的倍數：HPDMA 的資料寬度是 word，不是 4 的倍數硬體
     * 就報 USE。HAL 只在 >= 4 時幫忙進位，剩 1~3 個位元組時「Nothing to do」
     * 直接送進 DMA —— 那正是影片專案 26% 影格解碼失敗的原因
     * （board-notes 16.12）。往上取整是安全的：讀進來的緩衝區已經補過 0。 */
    n = (n + 3u) & ~3u;
    HAL_JPEG_ConfigInputBuffer((JPEG_HandleTypeDef *)hjpeg,
                               (uint8_t *)g_in_ptr, n);
}

/* 這兩個 photo.c 沒有定義（HAL 的 __weak 版本什麼都不做），所以直接放這裡
 * 不會撞名。影片沒在播時 g_done 沒人看，寫了也無害。 */
void HAL_JPEG_DecodeCpltCallback(JPEG_HandleTypeDef *hjpeg)
{
    (void)hjpeg;
    g_done = 1u;
}

void HAL_JPEG_ErrorCallback(JPEG_HandleTypeDef *hjpeg)
{
    (void)hjpeg;
    g_done = 2u;
}

void HPDMA1_Channel0_IRQHandler(void) { HAL_DMA_IRQHandler(&g_dma_in); }
void HPDMA1_Channel1_IRQHandler(void) { HAL_DMA_IRQHandler(&g_dma_out); }
void JPEG_IRQHandler(void)            { HAL_JPEG_IRQHandler(&g_hjpeg); }

/* ------------------------------------------------------------------ */
/* DMA 通道                                                            */
/* ------------------------------------------------------------------ */

/* 刻意**不**放進 HAL_JPEG_MspInit()。
 *
 * 第一版就是放在那裡，結果照片路徑每次初始化都會順便設定 DMA 通道與中斷，
 * 輪詢解碼被干擾成 93 次失敗 1 次成功，相簿完全不能用而且重燒也救不回來
 * （board-notes 16.3）。副作用要關在只有影片模式才會走的函式裡。 */
static bool dma_setup(void)
{
    __HAL_RCC_HPDMA1_CLK_ENABLE();      /* 漏開的話 HAL_DMA_Init 照樣回 OK */

    g_dma_out.Instance                   = HPDMA1_Channel1;
    g_dma_out.Init.Request               = HPDMA1_REQUEST_JPEG_TX;
    g_dma_out.Init.BlkHWRequest          = DMA_BREQ_SINGLE_BURST;
    g_dma_out.Init.Direction             = DMA_PERIPH_TO_MEMORY;
    g_dma_out.Init.SrcInc                = DMA_SINC_FIXED;
    g_dma_out.Init.DestInc               = DMA_DINC_INCREMENTED;
    g_dma_out.Init.SrcDataWidth          = DMA_SRC_DATAWIDTH_WORD;
    g_dma_out.Init.DestDataWidth         = DMA_DEST_DATAWIDTH_WORD;
    g_dma_out.Init.Priority              = DMA_LOW_PRIORITY_LOW_WEIGHT;
    g_dma_out.Init.SrcBurstLength        = 8;
    g_dma_out.Init.DestBurstLength       = 8;
    g_dma_out.Init.TransferAllocatedPort =
        DMA_SRC_ALLOCATED_PORT1 | DMA_DEST_ALLOCATED_PORT0;
    g_dma_out.Init.TransferEventMode     = DMA_TCEM_BLOCK_TRANSFER;
    g_dma_out.Init.Mode                  = DMA_NORMAL;
    if (HAL_DMA_Init(&g_dma_out) != HAL_OK) {
        return false;
    }
    __HAL_LINKDMA(&g_hjpeg, hdmaout, g_dma_out);

    g_dma_in.Instance                    = HPDMA1_Channel0;
    g_dma_in.Init.Request                = HPDMA1_REQUEST_JPEG_RX;
    g_dma_in.Init.BlkHWRequest           = DMA_BREQ_SINGLE_BURST;
    g_dma_in.Init.Direction              = DMA_MEMORY_TO_PERIPH;
    g_dma_in.Init.SrcInc                 = DMA_SINC_INCREMENTED;
    g_dma_in.Init.DestInc                = DMA_DINC_FIXED;
    g_dma_in.Init.SrcDataWidth           = DMA_SRC_DATAWIDTH_WORD;
    g_dma_in.Init.DestDataWidth          = DMA_DEST_DATAWIDTH_WORD;
    g_dma_in.Init.Priority               = DMA_LOW_PRIORITY_LOW_WEIGHT;
    g_dma_in.Init.SrcBurstLength         = 8;
    g_dma_in.Init.DestBurstLength        = 8;
    g_dma_in.Init.TransferAllocatedPort  =
        DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
    g_dma_in.Init.TransferEventMode      = DMA_TCEM_BLOCK_TRANSFER;
    g_dma_in.Init.Mode                   = DMA_NORMAL;
    if (HAL_DMA_Init(&g_dma_in) != HAL_OK) {
        return false;
    }
    __HAL_LINKDMA(&g_hjpeg, hdmain, g_dma_in);

    HAL_NVIC_SetPriority(HPDMA1_Channel0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(HPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(HPDMA1_Channel1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(HPDMA1_Channel1_IRQn);
    HAL_NVIC_SetPriority(JPEG_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(JPEG_IRQn);
    return true;
}

static void dma_teardown(void)
{
    /* 少做任何一步，之後的輪詢解碼都可能被影響（board-notes 16.3）。 */
    HAL_NVIC_DisableIRQ(HPDMA1_Channel0_IRQn);
    HAL_NVIC_DisableIRQ(HPDMA1_Channel1_IRQn);
    HAL_NVIC_DisableIRQ(JPEG_IRQn);
    (void)HAL_JPEG_Abort(&g_hjpeg);
    (void)HAL_DMA_DeInit(&g_dma_in);
    (void)HAL_DMA_DeInit(&g_dma_out);
    g_hjpeg.hdmain  = NULL;
    g_hjpeg.hdmaout = NULL;
    (void)HAL_JPEG_DeInit(&g_hjpeg);
}

/* ------------------------------------------------------------------ */
/* DMA2D 轉色                                                          */
/* ------------------------------------------------------------------ */

/* CPU 轉色實測佔每格 91%（64ms / 70ms），DMA2D 是 20.9ms —— 這是唯一值得
 * 動的地方（board-notes 16.9 / 16.11）。
 *
 * css 要對應過去：HAL 的 0=4:4:4、1=4:2:0、2=4:2:2，而 DMA2D 是
 * NO_CSS / CSS_420 / CSS_422，兩邊編號不一樣，照抄會錯得很難查。 */
static bool dma2d_setup(uint32_t jpeg_css)
{
    uint32_t css;

    if (jpeg_css == JPEG_420_SUBSAMPLING) {
        css = DMA2D_CSS_420;
    } else if (jpeg_css == JPEG_422_SUBSAMPLING) {
        css = DMA2D_CSS_422;
    } else {
        css = DMA2D_NO_CSS;
    }

    __HAL_RCC_DMA2D_CLK_ENABLE();

    g_hdma2d.Instance                      = DMA2D;
    g_hdma2d.Init.Mode                     = DMA2D_M2M_PFC;
    g_hdma2d.Init.ColorMode                = DMA2D_OUTPUT_RGB565;
    g_hdma2d.Init.OutputOffset             = 0;
    g_hdma2d.LayerCfg[1].InputOffset       = 0;
    g_hdma2d.LayerCfg[1].InputColorMode    = DMA2D_INPUT_YCBCR;
    g_hdma2d.LayerCfg[1].ChromaSubSampling = css;
    g_hdma2d.LayerCfg[1].AlphaMode         = DMA2D_NO_MODIF_ALPHA;
    g_hdma2d.LayerCfg[1].InputAlpha        = 0xFFu;

    if (HAL_DMA2D_Init(&g_hdma2d) != HAL_OK) {
        return false;
    }
    return HAL_DMA2D_ConfigLayer(&g_hdma2d, 1) == HAL_OK;
}

/* 每格直接寫暫存器啟動，不走 HAL_DMA2D_Start。
 *
 * HAL_DMA2D_Start 會把 State 設成 BUSY，而負責設回 READY 的
 * HAL_DMA2D_PollForTransfer 等待迴圈包在 if (CR & START) 裡、快速傳輸會被
 * 整個跳過（board-notes 3.3）。自己輪詢 START 的代價是 State 永遠停在
 * BUSY —— 第二次之後 HAL_DMA2D_Start 直接回 HAL_BUSY 什麼都不做，
 * 而回傳值被丟掉，完全沒有錯誤跡象（board-notes 16.10）。 */
static void dma2d_convert(uint8_t *dst, uint32_t w, uint32_t h)
{
    DMA2D->FGMAR = (uint32_t)VID_YCBCR;
    DMA2D->OMAR  = (uint32_t)dst;
    DMA2D->NLR   = (w << DMA2D_NLR_PL_Pos) | h;
    DMA2D->IFCR  = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;
    DMA2D->CR   |= DMA2D_CR_START;

    while ((DMA2D->CR & DMA2D_CR_START) != 0u) { }
    __DSB();
}

/* ------------------------------------------------------------------ */
/* 檔案                                                                */
/* ------------------------------------------------------------------ */

static bool hdr_ok(const hdr_t *h)
{
    return h->magic == VIDEO_MAGIC && h->count != 0u &&
           h->width == PHYS_W && h->height == PHYS_H &&
           (uint64_t)h->count * 8u <= VID_TBL_CAP &&
           h->max_size != 0u && ((h->max_size + 3u) & ~3u) <= VID_FRAME_CAP;
}

bool video_probe(const char *path, video_info_t *info)
{
    FIL   f;
    hdr_t h;
    UINT  got = 0;

    if (f_open(&f, path, FA_READ) != FR_OK) {
        return false;
    }
    if (f_read(&f, &h, sizeof(h), &got) != FR_OK || got != sizeof(h) ||
        !hdr_ok(&h)) {
        (void)f_close(&f);
        return false;
    }
    (void)f_close(&f);

    info->count    = h.count;
    info->width    = h.width;
    info->height   = h.height;
    info->fps_x100 = h.fps_x100 ? h.fps_x100 : 2400u;
    info->max_size = h.max_size;
    return true;
}

bool video_open(const video_info_t *info)
{
    UINT got = 0;

    video_close();                      /* 保證從乾淨狀態開始 */

    if (f_open(&g_fil, info->path, FA_READ) != FR_OK) {
        g_vdbg_lasterr = -1;
        return false;
    }
    g_open = true;

    /* 位移表開機讀一次就好，之後每格只要 f_lseek + f_read。 */
    if (f_lseek(&g_fil, VIDEO_HDR_BYTES) != FR_OK ||
        f_read(&g_fil, VID_TBL, info->count * 8u, &got) != FR_OK ||
        got != info->count * 8u) {
        g_vdbg_lasterr = -2;
        video_close();
        return false;
    }
    g_count = info->count;

    g_hjpeg.Instance = JPEG;
    if (HAL_JPEG_Init(&g_hjpeg) != HAL_OK) {
        g_vdbg_lasterr = -3;
        video_close();
        return false;
    }
    if (!dma_setup()) {
        g_vdbg_lasterr = -4;
        video_close();
        return false;
    }
    g_dma2d_ready = false;
    g_active      = true;
    /* 從這裡開始接管 JPEG 回呼。photo.c 沒註冊時走它原本的輪詢邏輯，
     * 註冊之後才轉交過來 —— 影片沒在播就完全等於不存在。 */
    photo_set_jpeg_hooks(video_jpeg_data_ready, video_jpeg_get_data);
    return true;
}

void video_close(void)
{
    /* 先解除回呼再拆硬體：拆的過程中 HAL 可能還會回呼一次，
     * 那時候如果還指向這裡，會對已經清掉的狀態動手。 */
    photo_set_jpeg_hooks(NULL, NULL);
    if (g_active) {
        dma_teardown();
        g_active = false;
    }
    if (g_open) {
        (void)f_close(&g_fil);
        g_open = false;
    }
    g_dma2d_ready = false;
    g_count       = 0;

    /* 把解碼器還給照片路徑。
     *
     * 上面的 DeInit 把週邊重置了，但 photo.c 自己那個 handle 仍然以為
     * 狀態是 READY —— 不重新初始化的話下一張照片會在一個沒初始化的週邊上
     * 解碼。photo_init() 做的正是 Init + 重建色彩表，重複呼叫是安全的。 */
    (void)photo_init();
}

/* ------------------------------------------------------------------ */
/* 解碼一格                                                            */
/* ------------------------------------------------------------------ */

static bool read_frame(uint32_t idx, uint32_t *out_len)
{
    uint32_t off    = VID_TBL[idx * 2u];
    uint32_t size   = VID_TBL[idx * 2u + 1u];
    uint32_t padded = (size + 3u) & ~3u;
    UINT     got    = 0;

    if (size == 0u || padded > VID_FRAME_CAP) {
        return false;
    }
    if (f_lseek(&g_fil, off) != FR_OK) {
        return false;
    }
    if (f_read(&g_fil, VID_FRAME, size, &got) != FR_OK || got != size) {
        return false;
    }
    /* 補到 4 的倍數並清成 0：餵給 DMA 的長度會往上取整，多讀到的必須是
     * 有效記憶體（board-notes 16.12）。自己補就不必依賴檔案裡的補齊。 */
    memset(VID_FRAME + size, 0, padded - size);
    *out_len = size;
    return true;
}

bool video_decode(uint32_t idx, uint8_t *dst)
{
    JPEG_ConfTypeDef info;
    uint32_t len = 0, first, c0;

    if (!g_active || idx >= g_count) {
        return false;
    }

    c0 = cyc_start();
    if (!read_frame(idx, &len)) {
        g_vdbg_lasterr = -10;
        g_vdbg_fail++;
        return false;
    }
    g_vdbg_us_read += cyc_us(c0);

    c0 = cyc_start();
    g_out_total = 0;
    g_in_ptr    = VID_FRAME;
    g_in_left   = len;
    g_done      = 0u;

    /* HAL 在啟動時是**捨去**（len - len%4）、續傳時是**進位**，兩邊規則
     * 不一致。這裡先進位，小影格就一次餵完不會有第二次啟動。 */
    first = (len > CHUNK) ? CHUNK : ((len + 3u) & ~3u);
    if (HAL_JPEG_Decode_DMA(&g_hjpeg, VID_FRAME, first,
                            VID_YCBCR, CHUNK) != HAL_OK) {
        g_vdbg_lasterr = -11;
        g_vdbg_fail++;
        return false;
    }
    {
        uint32_t t0 = HAL_GetTick();

        while (g_done == 0u) {
            if ((HAL_GetTick() - t0) > 2000u) {
                g_vdbg_lasterr = -12;
                /* 啟動了非同步硬體的失敗路徑一定要收乾淨，否則週邊停在忙碌
                 * 狀態，之後每次解碼都拿到 HAL_BUSY（board-notes 16.3）。 */
                (void)HAL_JPEG_Abort(&g_hjpeg);
                g_vdbg_fail++;
                return false;
            }
        }
    }

    /* 每格之間完整重新初始化。這是結論不是實驗：連續 N 格之後
     * HAL_JPEG_Decode_DMA 會永遠回傳 BUSY（相簿第 8 張、影片第 24 格），
     * 而 HAL_JPEG_Abort() 救不回來 —— 累積的是週邊狀態（board-notes 16.8）。 */
    (void)HAL_JPEG_DeInit(&g_hjpeg);
    g_hjpeg.Instance = JPEG;
    (void)HAL_JPEG_Init(&g_hjpeg);

    if (g_done != 1u) {
        g_vdbg_lasterr = -13;
        g_vdbg_fail++;
        return false;
    }
    g_vdbg_us_dec += cyc_us(c0);

    c0 = cyc_start();
    /* 取樣格式第一格才知道，這時候設定 DMA2D。 */
    if (!g_dma2d_ready) {
        if (HAL_JPEG_GetInfo(&g_hjpeg, &info) != HAL_OK) {
            g_vdbg_lasterr = -14;
            g_vdbg_fail++;
            return false;
        }
        g_dma2d_ready = dma2d_setup(info.ChromaSubsampling);
        if (!g_dma2d_ready) {
            g_vdbg_lasterr = -15;
            g_vdbg_fail++;
            return false;
        }
    }
    /* DMA2D 讀 YCbCr、寫 framebuffer 都繞過 D-Cache，不必先失效快取。 */
    dma2d_convert(dst, PHYS_W, PHYS_H);
    g_vdbg_us_cc += cyc_us(c0);

    g_vdbg_decoded++;
    return true;
}
