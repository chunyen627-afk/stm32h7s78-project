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
#include "ff.h"
#include "ff_gen_drv.h"
#include "stm32h7rsxx_hal_dts.h"

#include <stdbool.h>
#include <string.h>

/* 影格包有兩個來源，開機時自動判斷：
 *
 *   SD 卡 0:/video.bin  —— 長片走這裡。128MB 的外部 Flash 放不下五分鐘的
 *                          MJPEG（實測約 137MB），而 SD 卡沒有這個限制。
 *   外部 Flash 0x71000000 —— 短片走這裡。記憶體映射，可以就地讀、零複製，
 *                          是最快也最單純的路徑，拿來驗證管線很好用。
 *
 * 先試 SD，沒有卡或沒有檔案就自動退回 Flash。兩條路都保留的理由跟相簿
 * 那個「DMA 失敗就退回輪詢」一樣：讓一條路壞掉變成「換一條」而不是「不能用」。 */
#define FRAMES_BASE     ((const uint8_t *)0x71000000u)
#define FRAMES_MAGIC    0x32524656u          /* 'VFR2' 小端序 */
#define SD_VIDEO_PATH   "0:/video.bin"

/* 面板實體尺寸。影片專案不畫任何 UI，所以不引入繪圖層 —— 它會拉進
 * 中文字型表，對一個只顯示影格的韌體是純粹的負擔。 */
#define PHYS_W          800
#define PHYS_H          480

#define FB0_ADDR        LCD_LAYER_0_ADDRESS  /* 0x90000000 */
#define FB1_ADDR        LCD_LAYER_1_ADDRESS  /* 0x90200000 */

/* PSRAM：YCbCr 中間緩衝區。800x480 的 4:2:0 只要 576KB，配 2MB 有餘裕。 */
#define YCBCR_BUF       ((uint8_t *)0x90600000u)
#define YCBCR_CAP       (2u * 1024u * 1024u)

/* 從 SD 讀進來的單格 JPEG 放這裡。實測 800x480 q:v 5 每格約 19KB、
 * 最大 25KB，配 1MB 是為了以後換更高品質的素材也不用改。
 * Flash 來源不會用到這塊 —— 記憶體映射可以直接就地解碼。 */
#define FRAME_BUF       ((uint8_t *)0x90900000u)
#define FRAME_CAP       (1u * 1024u * 1024u)

/* 位移表。7200 格只要 57.6KB，配 2MB 可以到 26 萬格（約三小時）。 */
#define TBL_BUF         ((uint32_t *)0x90A00000u)
#define TBL_CAP         (2u * 1024u * 1024u)

/* HPDMA 的區塊大小欄位只有 16 位元，而 HAL 是 (size & 0xFFFF) 遮掉、不檢查。
 * 一次交出整塊會被默默截斷或直接變成 0，所以必須分塊餵（board-notes 16.2）。 */
#define CHUNK           (32u * 1024u)

/* 素材格率改成從檔頭讀，不再寫死 —— 不同影片的格率不一樣，寫死的話
 * 27fps 的素材用 24fps 播會愈跑愈慢。這個是退回用的預設值。 */
#define DEFAULT_FPS_X100  2400u

/* ---- 分帶轉色：YCbCr 中間層放內部 SRAM ---------------------------
 *
 * 瓶頸是 100MHz PSRAM 的頻寬不是運算（board-notes 16.11）。每格原本的
 * PSRAM 流量：
 *
 *   JPEG DMA 寫 YCbCr   576KB
 *   DMA2D   讀 YCbCr    576KB
 *   DMA2D   寫 RGB565   768KB   <- 這個省不掉，LTDC 要從 PSRAM 掃描
 *   -------------------------
 *   合計              1,920KB/格 = 24fps 下 46 MB/s，而 LTDC 同時也要 46
 *
 * 把 YCbCr 搬進內部 SRAM 就能砍掉前兩項的 1,152KB。
 * 問題是**整塊放不下**：800x480 的 4:2:0 要 562KB，而內部 AXI SRAM 只有
 * 456KB（連結腳本的 __RAM_SIZE = 0x72000）。
 *
 * 所以改成分帶：SRAM 裡只放幾條 MCU 列，解出一帶就立刻 DMA2D 轉進
 * framebuffer，576KB 從頭到尾不落 PSRAM。
 *
 * 一條 MCU 列 = 800/16 = 50 個 MCU，4:2:0 每個 MCU 384 bytes -> 19,200 bytes。
 * 480/16 = 30 條，取 2 條一帶剛好整除成 15 帶（不整除的話最後一帶要特別處理）。 */
#define MCU_ROW_BYTES   ((PHYS_W / 16u) * 384u)     /* 19200，只對 4:2:0 成立 */
#define BAND_MCU_ROWS   2u
#define BAND_BYTES      (BAND_MCU_ROWS * MCU_ROW_BYTES)
#define TOTAL_MCU_ROWS  (PHYS_H / 16u)              /* 30 */

/* ---- 為什麼分帶預設關閉 ----------------------------------------
 *
 * 分帶確實有效（實測轉色 19.91 -> 13.72ms、能力上限 29.1 -> 36.9 fps），
 * 但這個幾何**沒有合法的分帶大小**，兩個條件互相打架：
 *
 *  1. **一帶不能超過 65,535 bytes** —— HPDMA 的 BNDT 只有 16 位元，而 HAL 是
 *     `size & 0xFFFF` 遮掉、不檢查（board-notes 16.2）。更糟的是遮掉之後
 *     `JPEG_GET_DMA_REMAIN_DATA` 仍然用「原本的 OutDataLength 減 BNDT 餘量」
 *     回報長度 —— 也就是**回報 76,800、實際只寫了 11,264**，解碼不會報錯，
 *     畫面卻是大半舊資料。所以最多 3 條 MCU 列（57,600）。
 *
 *  2. **一帶的列數不能整除總列數** —— 整除時最後一塊輸出填滿的瞬間正好也是
 *     解碼結束，HAL 的收尾走不下去，解碼永遠不回報完成（board-notes 16.13）。
 *
 * 800x480 的 4:2:0 是 30 條 MCU 列，而 1、2、3 全都整除 30。無解。
 *
 * 先前 2 條列跑了 2184 格沒事是**僥倖**：那是個競爭，換一份影格檔（讀檔延遲
 * 不同）就現形 —— 實測換檔之後每格都在第 29 帶失敗（15 帶裡差最後一帶）。
 *
 * 所以預設關閉，程式碼保留給以後幾何不同時用（例如 720p 是 45 條，
 * 2 條一帶 = 38,400 bytes 又不整除，就成立）。 */

typedef struct {
    uint32_t magic, count, width, height, fps_x100, max_size;
} frames_hdr_t;

static JPEG_HandleTypeDef  g_hjpeg;
static DMA2D_HandleTypeDef g_hdma2d;
static bool                g_dma2d_ready;
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
volatile uint32_t g_dbg_late;            /* 跟不上素材格率的次數 */

/* 診斷開關，SWD 可寫：
 *   g_dbg_freeze  非 0 = 一直顯示第 0 格。真正靜止的畫面還在閃的話，
 *                 問題在 PSRAM/LTDC 層級；不閃就是格與格之間的問題。
 *   g_dbg_nopresent 非 0 = 不換頁，只解碼轉色。用來分離「顯示」與「產生內容」。 */
/* LTDC 的 FIFO 供不上（underrun）與傳輸錯誤。
 *
 * DMA2D 現在會突發地猛寫 PSRAM，而 LTDC 同時要每秒 60 次從同一塊 PSRAM
 * 讀完整個畫面 —— 相簿從來沒用過 DMA2D，這是全新的匯流排競爭。
 * 「整個畫面快速亮滅」正是 underrun 的典型症狀。
 *
 * 這些旗標是黏著的，開機瞬間發生過一次就會一直是 1，所以每格讀完就清掉、
 * 用計數跟格數對比才有意義（board-notes 10.5 那個假線索）。 */
/* 直接數「轉色寫進哪一塊」與「present 被呼叫幾次」。
 * OMAR 永遠是 FB1、g_front 卻會變 1，兩者矛盾 —— 不再靠推理。 */
volatile uint32_t g_dbg_to_fb0, g_dbg_to_fb1, g_dbg_present;

volatile uint32_t g_dbg_ltdc_fu;         /* FIFO underrun 次數 */
volatile uint32_t g_dbg_ltdc_te;         /* 傳輸錯誤次數 */

volatile uint32_t g_dbg_freeze;
volatile uint32_t g_dbg_nopresent;

/* ---- 解碼失敗的診斷（26% 的影格 jerr=ERROR_DMA）----------------------
 *
 * 已知的事實：失敗率兩次取樣都是 26.7%，除以圈數剛好是 120 格裡固定 32 格 ——
 * 也就是**確定性的、跟影格內容相關**，不是隨機競爭。所以要問的不是「什麼時候
 * 壞」而是「哪 32 格、它們有什麼共同點」。
 *
 * failmap/okmap 用位元記錄每個索引的結果（120 格 = 4 個字），這樣一次 SWD
 * 讀取就拿到完整的失敗集合，不必反覆試。
 *
 * snap[] 凍結在**第一次**失敗的當下，記錄 HAL 與 DMA 通道的實際設定。
 * USE（user setting error）是硬體在啟動時對設定的抱怨 —— 位址對齊、
 * 區塊長度與資料寬度不相容都會觸發，把 CSAR/CBR1 讀出來就知道是哪一個，
 * 不用在幾種可能之間猜（board-notes 16.4 就是靠這招一次定位的）。 */
volatile uint32_t g_dbg_failmap[4];
volatile uint32_t g_dbg_okmap[4];
volatile uint32_t g_dbg_snap[24];
volatile uint32_t g_dbg_snap_done;

static uint32_t       g_cur_idx;         /* 正在解的影格索引 */
static uint32_t       g_cur_size;
static const uint8_t *g_cur_jpg;
static uint32_t       g_in_cb;           /* 這格呼叫了幾次 GetDataCallback */
static uint32_t       g_out_cb;

static uint32_t g_next_ms;               /* 下一格該顯示的時刻 */

/* ---- 分帶狀態 ---------------------------------------------------- */

/* 這塊在內部 SRAM（.bss 落在 0x24000000 的 AXI SRAM）。CPU 從頭到尾不碰它 ——
 * JPEG DMA 寫、DMA2D 讀，兩者都繞過 D-Cache，所以不需要任何快取維護。 */
static uint8_t g_band[BAND_BYTES] __attribute__((aligned(32)));

static volatile uint32_t g_band_len;     /* 非 0 = 有一帶等著轉 */
static volatile uint8_t  g_band_paused;  /* 已經叫 HAL 暫停輸出 */
static uint32_t          g_band_y;       /* 下一帶要寫到第幾條掃描線 */
static uint8_t          *g_band_fb;      /* 目的 framebuffer */
static bool              g_banding;      /* 這一格走分帶 */
static bool              g_band_ok;      /* 格式對得上，可以分帶 */

/* SWD 可寫，用來 A/B：寫 0 就退回原本「整塊 YCbCr 放 PSRAM」的路徑，
 * 不用重編就能比較兩者的每格耗時。 */
/* 預設 0：見上面「為什麼分帶預設關閉」。設 1 只在幾何允許時才有意義。 */
volatile uint32_t g_dbg_useband;
/* 非 0 = 跳過 SD、直接用外部 Flash 的影格包。
 * PSRAM 時脈實驗要用：200MHz 下 PSRAM 讀取會出錯，而 SD 來源要把影格讀進
 * PSRAM 再解碼，輸入被弄壞就量不到速度。Flash 是記憶體映射、就地解碼，
 * 完全不經 PSRAM。 */
volatile uint32_t g_dbg_force_flash;
/* 非 0 = 不按素材格率對時，全速跑。量「能力上限」用 —— 對時開著的時候
 * 「等垂直消隱」那一欄是被對時相位決定的，不能拿來推算上限。 */
volatile uint32_t g_dbg_nopace;
volatile uint32_t g_dbg_bands;           /* 累計轉過幾帶 */
volatile uint32_t g_dbg_band_short;      /* 長度不是整條 MCU 列的次數 */

/* ---- 晶片接面溫度 ------------------------------------------------
 *
 * 板子上沒有外接溫度感測器，但 MCU 內建 DTS。量到的是**晶片接面溫度**，
 * 不是 PCB、PSRAM 或面板的溫度 —— 接面通常是全板最熱的地方，所以拿它
 * 當上限指標是合理的。
 *
 * 連續播放時 CPU、JPEG、DMA2D、LTDC、XSPI 全都在動，值得盯著。
 * 每 60 格量一次就夠，量測本身要等取樣完成，不要每格都做。 */
static DTS_HandleTypeDef g_hdts;
static bool              g_dts_ready;

volatile int32_t  g_dbg_temp_c;          /* 目前接面溫度（攝氏） */
volatile int32_t  g_dbg_temp_max;        /* 開機以來的最高值 */
volatile uint32_t g_dbg_temp_n;          /* 量測次數，確認它真的有在跑 */

static void dts_init(void)
{
    __HAL_RCC_DTS_CLK_ENABLE();

    g_hdts.Instance           = DTS;
    g_hdts.Init.QuickMeasure  = DTS_QUICKMEAS_DISABLE;  /* 要校正過的值 */
    g_hdts.Init.RefClock      = DTS_REFCLKSEL_PCLK;
    g_hdts.Init.TriggerInput  = DTS_TRIGGER_HW_NONE;    /* 軟體觸發就好 */
    g_hdts.Init.SamplingTime  = DTS_SMP_TIME_15_CYCLE;
    g_hdts.Init.Divider       = 0;
    g_hdts.Init.HighThreshold = 0;
    g_hdts.Init.LowThreshold  = 0;

    if (HAL_DTS_Init(&g_hdts) != HAL_OK) {
        return;
    }
    if (HAL_DTS_Start(&g_hdts) != HAL_OK) {
        return;
    }
    g_dts_ready = true;
    g_dbg_temp_max = -100;
}

static void dts_sample(void)
{
    int32_t t;

    if (!g_dts_ready) {
        return;
    }
    if (HAL_DTS_GetTemperature(&g_hdts, &t) != HAL_OK) {
        return;
    }
    g_dbg_temp_c = t;
    if (t > g_dbg_temp_max) {
        g_dbg_temp_max = t;
    }
    g_dbg_temp_n++;
}

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
    uint32_t end;
    uint32_t left;

    g_out_cb++;

    /* 分帶模式：不在這裡轉色。
     *
     * 轉一帶要幾百微秒，在回呼（中斷）裡忙等會把 SysTick 押後，HAL_GetTick
     * 就會漏拍，對時與逾時判斷全部失準。所以這裡只做兩件事：記下長度、
     * 叫 HAL 暫停輸出。真正的轉色交給主迴圈。
     *
     * 一定要 Pause：不暫停的話 HAL 會拿**同一塊**緩衝區重啟輸出 DMA，
     * 在我們還沒轉完的時候就蓋掉它（JPEG_DMAOutCpltCallback 只檢查
     * PAUSE_OUTPUT 旗標）。 */
    if (g_banding) {
        g_band_len    = OutDataLength;
        (void)HAL_JPEG_Pause(hjpeg, JPEG_PAUSE_RESUME_OUTPUT);
        g_band_paused = 1u;
        return;
    }

    end = (uint32_t)(pDataOut - YCBCR_BUF) + OutDataLength;
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

    g_in_cb++;
    if (NbDecodedData >= g_in_left) {
        g_in_left = 0;
        /* 沒有資料了要**明講**。HAL 的說明寫得很清楚：輸入結束時要用
         * InDataLength = 0 呼叫一次。直接 return 的話 pJpegInBuffPtr 與
         * InDataLength 都留著上一次的值，而 JPEG_DMAInCpltCallback 只檢查
         * 「InDataLength > 0」就重啟 DMA —— 等於把同一塊資料再送一次。 */
        HAL_JPEG_ConfigInputBuffer(hjpeg, (uint8_t *)g_in_ptr, 0u);
        return;
    }
    g_in_ptr  += NbDecodedData;
    g_in_left -= NbDecodedData;
    n = (g_in_left > CHUNK) ? CHUNK : g_in_left;

    /* 長度一定要是 4 的倍數。
     *
     * HPDMA 的來源／目的資料寬度都設成 word，BNDT 不是 4 的倍數硬體就報
     * USE（user setting error）。HAL 只在長度 >= 4 時幫忙進位，剩 1~3 個
     * 位元組時那條分支是「Nothing to do」，把 2 原封不動送進 DMA。
     *
     * 這就是 26% 影格解碼失敗的全部原因：JPEG 核心消耗的量永遠是 4 的倍數，
     * 所以整包一次餵完的小影格會在最後剩下 size%4 個位元組。實測 48 張單塊
     * 影格裡 size%4!=0 的 32 張**每一次都失敗**、size%4==0 的 16 張全過，
     * 72 張要分塊的大影格因為尾段夠長（HAL 會進位）也全過 —— 32/120 = 26.7%。
     *
     * 往上取整是安全的：packframes.py 把每格補齊到 4 位元組邊界，多讀到的
     * 是補的 0，而且落在 EOI 之後，解碼器不會理它。 */
    n = (n + 3u) & ~3u;
    HAL_JPEG_ConfigInputBuffer(hjpeg, (uint8_t *)g_in_ptr, n);
}

void HAL_JPEG_DecodeCpltCallback(JPEG_HandleTypeDef *hjpeg) { (void)hjpeg; g_done = 1u; }

void HAL_JPEG_ErrorCallback(JPEG_HandleTypeDef *hjpeg)
{
    g_done = 2u;

    /* 只留第一次的現場。之後的失敗會覆蓋掉最有價值的那一筆，而且
     * 錯誤碼在後續的 DeInit/Init 會被清成 0（board-notes 已經吃過這個虧）。 */
    if (g_dbg_snap_done != 0u) {
        return;
    }
    g_dbg_snap[0]  = g_cur_idx;
    g_dbg_snap[1]  = g_cur_size;
    g_dbg_snap[2]  = g_in_cb;
    g_dbg_snap[3]  = g_out_cb;
    g_dbg_snap[4]  = (uint32_t)(g_in_ptr - g_cur_jpg);   /* 輸入消耗到哪 */
    g_dbg_snap[5]  = g_in_left;
    g_dbg_snap[6]  = g_out_total;
    g_dbg_snap[7]  = hjpeg->InDataLength;
    g_dbg_snap[8]  = (uint32_t)hjpeg->pJpegInBuffPtr;
    g_dbg_snap[9]  = hjpeg->OutDataLength;
    g_dbg_snap[10] = (uint32_t)hjpeg->pJpegOutBuffPtr;
    g_dbg_snap[11] = hjpeg->ErrorCode;
    g_dbg_snap[12] = (uint32_t)hjpeg->State;
    g_dbg_snap[13] = hjpeg->Context;
    /* 輸入通道（HPDMA1_Channel0）的實際設定：USE 就是硬體對這幾個值的抱怨 */
    g_dbg_snap[14] = HPDMA1_Channel0->CSAR;
    g_dbg_snap[15] = HPDMA1_Channel0->CDAR;
    g_dbg_snap[16] = HPDMA1_Channel0->CBR1;
    g_dbg_snap[17] = HPDMA1_Channel0->CTR1;
    g_dbg_snap[18] = HPDMA1_Channel0->CSR;
    /* 輸出通道（HPDMA1_Channel1） */
    g_dbg_snap[19] = HPDMA1_Channel1->CDAR;
    g_dbg_snap[20] = HPDMA1_Channel1->CBR1;
    g_dbg_snap[21] = HPDMA1_Channel1->CSR;
    g_dbg_snap[22] = g_dma_in.ErrorCode;
    g_dbg_snap[23] = g_dma_out.ErrorCode;
    g_dbg_snap_done = 1u;
}

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
/* DMA2D 轉色                                                          */
/* ------------------------------------------------------------------ */

/* YCbCr -> RGB565 交給硬體。
 *
 * 實測 CPU 轉色佔每格 91%（64ms / 70ms），解碼只佔 3.6% —— 這是唯一值得
 * 動的地方。DMA2D 支援 YCbCr 輸入（DMA2D_INPUT_YCBCR + 三種色度取樣），
 * 而 JPEG 解碼器吐出來的正是它要的 MCU 區塊排列，兩者直接接得上。
 *
 * 附帶好處：DMA2D 讀 YCbCr、寫 framebuffer 都繞過 D-Cache，所以轉色前
 * 那次 SCB_InvalidateDCache_by_Addr（CPU 要讀才需要）可以省掉。
 *
 * css 用 JPEG 回報的取樣格式對應過去：HAL 的 0=4:4:4、1=4:2:0、2=4:2:2，
 * 而 DMA2D 是 NO_CSS / CSS_420 / CSS_422 —— 兩邊的編號不一樣，
 * 照抄會錯得很難查（board-notes 11.1 那個 4:4:4 誤認的同一類陷阱）。 */
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

    g_hdma2d.Instance                 = DMA2D;
    g_hdma2d.Init.Mode                = DMA2D_M2M_PFC;   /* 搬運 + 格式轉換 */
    g_hdma2d.Init.ColorMode           = DMA2D_OUTPUT_RGB565;
    g_hdma2d.Init.OutputOffset        = 0;               /* 影格就是整個畫面 */

    g_hdma2d.LayerCfg[1].InputOffset        = 0;
    g_hdma2d.LayerCfg[1].InputColorMode     = DMA2D_INPUT_YCBCR;
    g_hdma2d.LayerCfg[1].ChromaSubSampling  = css;
    g_hdma2d.LayerCfg[1].AlphaMode          = DMA2D_NO_MODIF_ALPHA;
    g_hdma2d.LayerCfg[1].InputAlpha         = 0xFFu;

    if (HAL_DMA2D_Init(&g_hdma2d) != HAL_OK) {
        return false;
    }
    if (HAL_DMA2D_ConfigLayer(&g_hdma2d, 1) != HAL_OK) {
        return false;
    }
    return true;
}

/* 每格直接寫暫存器啟動，不走 HAL_DMA2D_Start。
 *
 * 為什麼：HAL_DMA2D_Start() 會把 hdma2d.State 設成 BUSY，而負責設回 READY 的
 * 是 HAL_DMA2D_PollForTransfer()。但那個函式的等待迴圈包在 if (CR & START) 裡，
 * 傳輸太快時整個等待會被跳過（board-notes 3.3），所以我們改成自己輪詢 START。
 * 代價是 State 永遠停在 BUSY —— **第二次之後 HAL_DMA2D_Start 直接回傳
 * HAL_BUSY 什麼都不做**。
 *
 * 症狀非常具人欺騙性：程式邏輯正確地輪流指定兩塊 framebuffer（實測
 * to_fb0=255、to_fb1=259），但硬體的 OMAR 永遠停在第一次那個位址，
 * 另一塊從頭到尾是黑的 —— 畫面就在第一格與全黑之間交替閃爍。
 * 回傳值被 (void) 丟掉，所以完全沒有錯誤跡象。
 *
 * 相簿的 dma2d.c 早就是直接寫暫存器（board-notes 3.3），這裡照做。
 * Init/ConfigLayer 仍用 HAL，那是一次性的、不涉及狀態機。 */
static void dma2d_convert(uint8_t *dst, uint32_t w, uint32_t h)
{
    DMA2D->FGMAR = (uint32_t)YCBCR_BUF;
    DMA2D->OMAR  = (uint32_t)dst;
    DMA2D->NLR   = (w << DMA2D_NLR_PL_Pos) | h;
    DMA2D->IFCR  = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;
    DMA2D->CR   |= DMA2D_CR_START;

    /* 看 START 位不要看旗標。 */
    while ((DMA2D->CR & DMA2D_CR_START) != 0u) { }
    __DSB();
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
    /* g_front 是「目前顯示中」的索引，所以剛畫好的是另一塊。
     * 這裡顯示剛畫好的那塊，然後翻面 —— 繪製目標與這裡必須永遠相反，
     * 寫反的話每格都在顯示沒畫過的緩衝區，靜止畫面也會閃（board-notes 2.3）。 */
    {
        uint32_t isr = LTDC->ISR;

        if ((isr & LTDC_ISR_FUIF) != 0u) { g_dbg_ltdc_fu++; }
        if ((isr & LTDC_ISR_TERRIF) != 0u) { g_dbg_ltdc_te++; }
        LTDC->ICR = LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF;
    }

    g_dbg_present++;
    LTDC_Layer1->CFBAR = g_front ? FB0_ADDR : FB1_ADDR;
    LTDC->SRCR = LTDC_SRCR_VBR;
    for (uint32_t g = 0; g < 2000000u; g++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) { break; }
    }
    g_front ^= 1u;
}

/* 轉一帶：從 SRAM 讀 YCbCr，寫進 framebuffer 對應的那幾條掃描線。 */
static void dma2d_band(uint8_t *dst, const uint8_t *src, uint32_t lines)
{
    DMA2D->FGMAR = (uint32_t)src;
    DMA2D->OMAR  = (uint32_t)dst;
    DMA2D->NLR   = (PHYS_W << DMA2D_NLR_PL_Pos) | lines;
    DMA2D->IFCR  = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;
    DMA2D->CR   |= DMA2D_CR_START;

    while ((DMA2D->CR & DMA2D_CR_START) != 0u) { }
    __DSB();
}

/* 把等著的那一帶轉掉。主迴圈呼叫，不在中斷裡。 */
static void band_drain(void)
{
    uint32_t len = g_band_len;
    uint32_t lines;
    uint32_t c0;

    if (len == 0u) {
        return;
    }
    /* 只處理整條 MCU 列。餘數代表對不上預期的版面，寧可丟掉那一小段
     * 也不要拿錯的行數去寫 framebuffer（寫超過就踩到下一塊緩衝區）。 */
    lines = (len / MCU_ROW_BYTES) * 16u;
    if ((len % MCU_ROW_BYTES) != 0u) {
        g_dbg_band_short++;
    }
    if (lines != 0u && (g_band_y + lines) <= PHYS_H) {
        c0 = cyc_start();
        dma2d_band(g_band_fb + (size_t)g_band_y * PHYS_W * 2u, g_band, lines);
        g_dbg_sum_cc += cyc_us(c0);
        g_band_y     += lines;
        g_dbg_bands++;
    }
    g_band_len = 0u;
}

/* ------------------------------------------------------------------ */
/* 影格來源：SD 卡優先，退回外部 Flash                                   */
/* ------------------------------------------------------------------ */

extern const Diskio_drvTypeDef SD_BSP_Driver;
bool sd_writer_requested(void);
void sd_writer_run(void);

static FATFS        g_fs;
static FIL          g_fil;
static char         g_drive[4];
static bool         g_src_sd;
static bool         g_fs_mounted;
static frames_hdr_t g_hdr;
static const uint32_t *g_tbl;     /* Flash：指進映射區；SD：指向 TBL_BUF */

volatile uint32_t g_dbg_src;      /* 0=都沒有 1=SD 2=Flash */
volatile int32_t  g_dbg_fs_err;   /* 最後一次 FatFs 的錯誤碼 */
volatile uint32_t g_dbg_sum_read; /* 從 SD 讀影格的累計微秒 */

static bool hdr_valid(const frames_hdr_t *h)
{
    return h->magic == FRAMES_MAGIC && h->count != 0u &&
           h->width == PHYS_W && h->height == PHYS_H &&
           (uint64_t)h->count * 8u <= TBL_CAP &&
           h->max_size != 0u && ((h->max_size + 3u) & ~3u) <= FRAME_CAP;
}

/* SD：把檔頭與位移表讀進 PSRAM，影格本體留在卡上按需讀取。
 * 位移表 7200 格只有 57.6KB，開機讀一次就好，之後每格只要 f_lseek + f_read。 */
static bool src_open_sd(void)
{
    UINT got = 0;

    if (FATFS_LinkDriver(&SD_BSP_Driver, g_drive) != 0) {
        return false;
    }
    /* 第三個參數 1 = 立刻掛載。延後掛載會讓錯誤拖到第一次 f_open 才出現。 */
    g_dbg_fs_err = (int32_t)f_mount(&g_fs, g_drive, 1);
    if (g_dbg_fs_err != FR_OK) {
        return false;
    }
    g_fs_mounted = true;

    g_dbg_fs_err = (int32_t)f_open(&g_fil, SD_VIDEO_PATH, FA_READ);
    if (g_dbg_fs_err != FR_OK) {
        return false;
    }
    if (f_read(&g_fil, &g_hdr, sizeof(g_hdr), &got) != FR_OK ||
        got != sizeof(g_hdr) || !hdr_valid(&g_hdr)) {
        (void)f_close(&g_fil);
        return false;
    }
    if (f_read(&g_fil, TBL_BUF, g_hdr.count * 8u, &got) != FR_OK ||
        got != g_hdr.count * 8u) {
        (void)f_close(&g_fil);
        return false;
    }
    g_tbl   = TBL_BUF;
    g_src_sd = true;
    return true;
}

/* Flash：記憶體映射，檔頭與表格直接就地指過去，零複製。 */
static bool src_open_flash(void)
{
    const frames_hdr_t *h = (const frames_hdr_t *)FRAMES_BASE;

    if (!hdr_valid(h)) {
        return false;
    }
    g_hdr   = *h;
    g_tbl   = (const uint32_t *)(FRAMES_BASE + sizeof(frames_hdr_t));
    g_src_sd = false;
    return true;
}

/* 取一格。回傳的指標可以直接餵進 JPEG 解碼器。 */
static const uint8_t *src_frame(uint32_t idx, uint32_t *len)
{
    uint32_t off  = g_tbl[idx * 2u];
    uint32_t size = g_tbl[idx * 2u + 1u];
    uint32_t padded = (size + 3u) & ~3u;
    UINT     got = 0;
    uint32_t c0;

    *len = size;
    if (!g_src_sd) {
        return FRAMES_BASE + off;       /* 映射區就地解碼，不必複製 */
    }

    if (padded > FRAME_CAP) {
        return NULL;
    }
    c0 = cyc_start();
    if (f_lseek(&g_fil, off) != FR_OK) {
        return NULL;
    }
    if (f_read(&g_fil, FRAME_BUF, size, &got) != FR_OK || got != size) {
        return NULL;
    }
    /* 補到 4 的倍數並清成 0。解碼器那邊的長度會往上取整（board-notes 16.12），
     * 多讀到的必須是有效記憶體 —— 這裡自己補，就不必依賴檔案裡的補齊。 */
    memset(FRAME_BUF + size, 0, padded - size);
    g_dbg_sum_read += cyc_us(c0);
    return FRAME_BUF;
}

/* 播放中隨時可以被主機叫進燒錄模式。先把自己的檔案收乾淨再交出去。 */
static void check_writer(void)
{
    if (!sd_writer_requested()) {
        return;
    }
    if (g_src_sd) {
        (void)f_close(&g_fil);
    }
    if (g_fs_mounted) {
        (void)f_mount(NULL, g_drive, 0);
        g_fs_mounted = false;
    }
    g_dbg_stage = 20;
    sd_writer_run();                    /* 不會返回 */
}

/* ------------------------------------------------------------------ */
/* 播放                                                                */
/* ------------------------------------------------------------------ */

static bool decode_frame(const uint8_t *jpg, uint32_t size, uint8_t *dst)
{
    uint32_t first;

    g_out_total = 0;
    g_in_ptr    = jpg;
    g_in_left   = size;
    g_done      = 0u;
    g_cur_jpg   = jpg;
    g_cur_size  = size;
    g_in_cb     = 0u;
    g_out_cb    = 0u;

    /* 只有在「格式確定對得上」時才走分帶：MCU_ROW_BYTES 是照 4:2:0、寬 800
     * 算死的，格式一變那個數字就錯，而錯的行數會寫超出 framebuffer。
     * 第一格 g_dma2d_ready 還是 false，所以一定走原本的整塊路徑，
     * 順便把格式量出來。 */
    g_banding = (g_dbg_useband != 0u) && g_dma2d_ready && g_band_ok &&
                (dst != NULL);
    if (g_banding) {
        g_band_len    = 0u;
        g_band_paused = 0u;
        g_band_y      = 0u;
        g_band_fb     = dst;
    }

    /* 第一塊也要是 4 的倍數。HAL 在啟動時的處理是**捨去**
     * （`InDataLength - InDataLength % 4`），續傳時卻是**進位** —— 兩邊規則
     * 不一致。捨去會把尾巴留給下一次，而那個尾巴正好是 1~3 個位元組時就
     * 觸發 USE。這裡先進位，小影格就一次餵完，不會有第二次啟動。 */
    first = (size > CHUNK) ? CHUNK : ((size + 3u) & ~3u);
    if (HAL_JPEG_Decode_DMA(&g_hjpeg, (uint8_t *)jpg, first,
                            g_banding ? g_band : YCBCR_BUF,
                            g_banding ? BAND_BYTES : CHUNK) != HAL_OK) {
        g_dbg_lasterr = -3;
        return false;
    }
    {
        uint32_t t0 = HAL_GetTick();

        while (g_done == 0u) {
            /* 分帶模式：回呼填滿一帶就會暫停輸出，主迴圈在這裡把它轉掉
             * 再餵回同一塊、恢復輸出。轉色因此跟解碼交錯進行，而 YCbCr
             * 從頭到尾只存在於內部 SRAM。 */
            if (g_banding && g_band_len != 0u) {
                band_drain();
                if (g_band_paused != 0u) {
                    g_band_paused = 0u;
                    HAL_JPEG_ConfigOutputBuffer(&g_hjpeg, g_band, BAND_BYTES);
                    (void)HAL_JPEG_Resume(&g_hjpeg, JPEG_PAUSE_RESUME_OUTPUT);
                }
            }
            if ((HAL_GetTick() - t0) > 2000u) {
                g_dbg_lasterr = -4;
                (void)HAL_JPEG_Abort(&g_hjpeg);
                g_banding = false;
                return false;
            }
        }
    }
    /* 解碼結束時 HAL 還會回呼一次，最後一帶留在這裡等著轉。 */
    if (g_banding) {
        band_drain();
        g_band_paused = 0u;
    }
    /* 錯誤碼一定要在 DeInit 之前擷取 —— 重新初始化會把 ErrorCode 清成 0，
     * 先前 jerr/derr 全是 0 就是這個順序寫錯造成的假象。 */
    if (g_done != 1u) {
        g_dbg_jerr     = HAL_JPEG_GetError(&g_hjpeg);
        g_dbg_derr_in  = HAL_DMA_GetError(&g_dma_in);
        g_dbg_derr_out = HAL_DMA_GetError(&g_dma_out);
    }

    /* 每格之間完整重新初始化 JPEG。這是結論不是實驗（board-notes 16.8）。
     *
     * 症狀：連續 N 格之後 HAL_JPEG_Decode_DMA 永遠回傳 BUSY（相簿是第 8 張、
     * 這裡是第 24 格），而且 HAL_JPEG_Abort() 救不回來 —— 兩邊都試過。
     * 所以累積的是週邊/HAL 的狀態，不是單次操作沒收乾淨。
     * 加上這段之後可以連續播放上千格不停。
     * 成本每格幾十微秒，相對整格 31ms 可以忽略。 */
    (void)HAL_JPEG_DeInit(&g_hjpeg);
    g_hjpeg.Instance = JPEG;
    (void)HAL_JPEG_Init(&g_hjpeg);

    /* 這個 Abort 是當初驗證「永久 BUSY」假設時加的，上面的 DeInit+Init 才是
     * 真正的解法，它其實已經是多餘的（而且會把 g_dma_* 的 ErrorCode 弄髒成
     * NO_XFER，干擾之後的診斷）。先留著不動：目前這條路徑實測 1940 格 0 失敗，
     * 要拿掉應該單獨改、單獨量，不要跟修正混在同一次。 */
    (void)HAL_JPEG_Abort(&g_hjpeg);

    if (g_done != 1u) {
        g_dbg_lasterr   = -5;
        /* 啟動了非同步硬體的失敗路徑一定要收乾淨，否則週邊停在忙碌狀態，
         * 之後每次解碼都拿到 HAL_BUSY（board-notes 16.3）。 */
        (void)HAL_JPEG_Abort(&g_hjpeg);
        return false;
    }
    return true;
}

void video_run(void)
{
    JPEG_ConfTypeDef    info;
    JPEG_YCbCrToRGB_Convert_Function convert;
    uint32_t nb_mcu = 0, converted = 0;
    uint32_t idx = 0, t_run;
    uint32_t frame_us, frame_ms, frame_frac, acc = 0;

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
    dts_init();

    /* 先試 SD，沒有就退回 Flash。
     *
     * g_dbg_force_flash 是給 PSRAM 時脈實驗用的：200MHz 下 PSRAM 讀取會出錯，
     * 而 SD 來源要把影格讀進 PSRAM 再解碼 —— 輸入被弄壞，量到的就不是速度
     * 而是失敗。改走 Flash（記憶體映射、就地解碼、完全不經 PSRAM）之後，
     * 只剩 framebuffer 的寫入會壞：畫面是雜訊，但時間量測仍然有效。 */
    g_dbg_stage = 2;
    if (g_dbg_force_flash) {
        g_dbg_src = src_open_flash() ? 2u : 0u;
    } else if (src_open_sd()) {
        g_dbg_src = 1;
    } else if (src_open_flash()) {
        g_dbg_src = 2;
    } else {
        g_dbg_src = 0;
    }

    g_dbg_hdr_magic = g_hdr.magic;
    g_dbg_hdr_count = g_hdr.count;
    g_dbg_hdr_w     = g_hdr.width;
    g_dbg_hdr_h     = g_hdr.height;

    if (g_dbg_src == 0u) {
        /* 兩個來源都沒有 —— 這是第一次使用的正常狀態（卡上還沒有 video.bin）。
         * **絕對不能停在這裡空轉**：主機要靠這個迴圈把板子叫進燒錄模式，
         * 卡死的話就變成「要有影片才裝得進影片」的雞生蛋問題。 */
        g_dbg_stage = 91;
        for (;;) {
            check_writer();
        }
    }

    /* 格率從檔頭讀。用微秒累加而不是「每格睡固定毫秒」—— HAL_GetTick 只有
     * 1ms 解析度，24fps 的 41.67ms 無論取 41 還是 42 都會慢慢漂掉。
     * 整數部分照加，小數部分累積到 1ms 就補一格。 */
    frame_us   = 100000000u / (g_hdr.fps_x100 ? g_hdr.fps_x100 : DEFAULT_FPS_X100);
    frame_ms   = frame_us / 1000u;
    frame_frac = frame_us % 1000u;

    g_dbg_stage = 5;
    t_run     = HAL_GetTick();
    g_next_ms = t_run;

    for (;;) {
        uint32_t       use = g_dbg_freeze ? 0u : idx;
        const uint8_t *jpg;
        uint32_t       len = 0;
        uint32_t       c_all = cyc_start();
        uint32_t       c0;

        check_writer();

        g_cur_idx = use;
        jpg = src_frame(use, &len);
        if (jpg == NULL) {
            g_dbg_fail++;
            idx = (idx + 1u) % g_hdr.count;
            continue;
        }

        {
            uint32_t cc0 = g_dbg_sum_cc;   /* 分帶時轉色發生在解碼期間 */

        c0 = cyc_start();
        if (!decode_frame(jpg, len, (uint8_t *)(g_front ? FB0_ADDR : FB1_ADDR))) {
            g_dbg_fail++;
            if (use < 128u) {
                g_dbg_failmap[use >> 5] |= 1u << (use & 31u);
            }
            idx = (idx + 1u) % g_hdr.count;
            continue;
        }
        if (use < 128u) {
            g_dbg_okmap[use >> 5] |= 1u << (use & 31u);
        }
        /* 扣掉分帶轉色的時間，解碼那欄才還是「純解碼」，跟舊數字可比。 */
        g_dbg_sum_dec += cyc_us(c0) - (g_dbg_sum_cc - cc0);
        }

        if (HAL_JPEG_GetInfo(&g_hjpeg, &info) != HAL_OK ||
            JPEG_GetDecodeColorConvertFunc(&info, &convert, &nb_mcu) != HAL_OK) {
            g_dbg_fail++;
            idx = (idx + 1u) % g_hdr.count;
            continue;
        }

        /* 第一格才知道取樣格式，這時候設定 DMA2D。 */
        if (!g_dma2d_ready) {
            g_dma2d_ready = dma2d_setup(info.ChromaSubsampling);
            g_dbg_stage = g_dma2d_ready ? 6u : 92u;
            /* MCU_ROW_BYTES 是照 4:2:0、寬 800 算死的，格式不合就不能分帶。 */
            g_band_ok = (info.ChromaSubsampling == JPEG_420_SUBSAMPLING) &&
                        (info.ImageWidth == (uint32_t)PHYS_W) &&
                        (info.ImageHeight == (uint32_t)PHYS_H);
        }

        c0 = cyc_start();
        if (g_front) { g_dbg_to_fb0++; } else { g_dbg_to_fb1++; }
        if (g_banding) {
            /* 已經在解碼期間一帶一帶轉完了，這裡什麼都不用做。 */
        } else if (g_dma2d_ready) {
            /* DMA2D 讀寫都繞過 D-Cache，不必先失效快取。 */
            dma2d_convert((uint8_t *)(g_front ? FB0_ADDR : FB1_ADDR),
                          info.ImageWidth, info.ImageHeight);
        } else {
            /* 退回 CPU 轉色。這條路徑實測 64ms/格，只當保險用。 */
            SCB_InvalidateDCache_by_Addr((uint32_t *)YCBCR_BUF,
                                         (int32_t)g_out_total);
            (void)convert(YCBCR_BUF,
                          (uint8_t *)(g_front ? FB0_ADDR : FB1_ADDR),
                          0, g_out_total, &converted);
        }
        g_dbg_sum_cc += cyc_us(c0);

        c0 = cyc_start();
        if (!g_dbg_nopresent) {
            present();
        }
        g_dbg_sum_out += cyc_us(c0);

        /* 按素材的節奏放。用「下一格的目標時刻」而不是「每格睡固定時間」，
         * 否則解碼時間的抖動會累積成愈跑愈慢。 */
        g_next_ms += frame_ms;
        acc += frame_frac;
        if (acc >= 1000u) {             /* 小數毫秒滿一格就補回去 */
            acc -= 1000u;
            g_next_ms += 1u;
        }
        {
            uint32_t now = HAL_GetTick();

            if (g_dbg_nopace) {
                g_next_ms = now;    /* 不對時：量真實的能力上限 */
            } else if ((int32_t)(g_next_ms - now) > 0) {
                while ((int32_t)(g_next_ms - HAL_GetTick()) > 0) { }
            } else {
                /* 跟不上就重新對時，不要一直追欠下的時間。 */
                g_next_ms = now;
                g_dbg_late++;
            }
        }

        g_dbg_sum_all += cyc_us(c_all);
        g_dbg_nframe++;

        /* 每 60 格更新一次實測格率，用牆鐘時間算才包含所有開銷。 */
        if ((g_dbg_nframe % 60u) == 0u) {
            uint32_t ms = HAL_GetTick() - t_run;

            if (ms > 0u) {
                g_dbg_fps_x100 = (g_dbg_nframe * 100000u) / ms;
            }
            dts_sample();
        }
        idx = (idx + 1u) % g_hdr.count;
    }
}
