/**
 * 一張照片從 SD 卡到面板的完整路徑。
 *
 *   讀檔 -> 硬體 JPEG 解碼 -> YCbCr 轉 RGB888
 *        -> 盒狀濾波縮放（8 位元精度）-> 銳化 -> 抖動量化成 RGB565 並旋轉
 *
 * 幾個關鍵決定：
 *
 * 1. 解碼成 RGB888 而不是面板原生的 RGB565。多花一個 byte，換來整條後製都在
 *    8 位元精度下進行；一開始就砍成 5/6/5，後面再怎麼處理都補不回來。
 *
 * 2. 縮放用盒狀濾波（把目的像素涵蓋到的來源像素全部平均），不是最近鄰。
 *    1200x1800 縮到 480x800 是 2.25 倍，最近鄰等於每 5 個像素丟掉 4 個，
 *    細節流失而且邊緣會有鋸齒。
 *
 * 3. 縮小之後補一次銳化。任何正確的縮小都會讓影像變軟（高頻被平均掉了），
 *    在 187 PPI 的面板上特別明顯，補回來才有「清楚」的感覺。
 *
 * 4. 最後一步才量化到 RGB565，而且加上有序抖動。直接截斷會讓膚色、天空這種
 *    平滑漸層出現色帶，抖動用空間雜訊把量化誤差打散，肉眼看起來連續很多。
 *
 * 5. 旋轉折進最後那一次寫入。面板是 800x480 橫式、照片是直式，不轉的話只能
 *    縮到 320x480；轉成直立可以填滿 480x800，面積是 2.25 倍。
 */
#include "main.h"
#include "photo.h"
#include "gfx.h"

#include "ff.h"
#include "jpeg_utils.h"

#include <math.h>
#include <string.h>

/* PSRAM 配置（總共 32MB：0x90000000 ~ 0x91FFFFFF）
 *
 *   0x90000000  FB0 + FB1      4 MB   framebuffer（BSP 固定）
 *   0x90400000  JPEG 原始檔    2 MB
 *   0x90600000  YCbCr         10 MB   ← 必須裝得下 4:4:4（見下）
 *   0x91000000  RGB888        10 MB
 *   0x91A00000  縮圖 RGB888    2 MB   實際只用 480x800x3 = 1.15MB
 *   0x91C00000  播放清單       2 MB   見 album_main.c
 *   0x91E00000  資料夾索引     8 KB＋餘裕
 *
 * YCbCr 的大小踩過一個大坑：一開始配 6MB，是照 4:2:0（每像素 1.5 bytes）
 * 估的。但相機直出的 JPEG 很多是 4:4:4 —— 每像素 3 bytes，1800x1200 就要
 * 6.48MB，裝不下。HAL 的行為是「輸出緩衝區滿了就繞回開頭繼續寫」：
 * 照片尾端的 MCU 蓋掉開頭的 MCU，畫面上就是「照片底部跑到最上面、
 * 底部留著上一張的殘影」。所以這塊必須以 4:4:4 的最壞情況來配。
 */
/* 位移統一從 PSRAM_BASE 算起，這樣 PC 上的測試台可以把整塊指到 malloc 出來的
 * 記憶體（-DPSRAM_BASE=...），直接編譯這個檔案來驗證縮放與後製的畫面品質，
 * 不必把演算法重寫一份 —— 重寫的話比較的就不是板子上實際跑的程式了。
 * 韌體端不定義這個巨集，就還是原本的 0x90000000。 */
#ifndef PSRAM_BASE
#define PSRAM_BASE      0x90000000u
#endif

#define JPEG_FILE_BUF   ((uint8_t *)(PSRAM_BASE + 0x00400000u))
#define JPEG_FILE_CAP   (2u * 1024u * 1024u)
#define YCBCR_BUF       ((uint8_t *)(PSRAM_BASE + 0x00600000u))
#define YCBCR_CAP       (10u * 1024u * 1024u)
#define RGB_BUF         ((uint8_t *)(PSRAM_BASE + 0x01000000u))   /* RGB888 全尺寸 */
#define RGB_CAP         (10u * 1024u * 1024u)
#define SCALED_BUF      ((uint8_t *)(PSRAM_BASE + 0x01A00000u))   /* RGB888 縮圖 */
#define SCALED_CAP      (2u * 1024u * 1024u)

/* 填滿畫面時容許裁掉的最大比例，超過就退回完整顯示（留黑邊）。
 *
 * 設 0 = 永不裁切，任何照片都完整顯示。
 *
 * 原本是 25，理由是「2:3 只需裁 10%，25 足以擋掉全景照那種會被砍一半的」。
 * 但實際用起來，會被裁到的正是最常見的那幾種比例：3:4 切掉 20%（左右各 10%）、
 * 2:3 切掉 10%（左右各 5%）—— 相簿的用途是看照片，拿內容換版面划不來。
 *
 * 黑邊沒有想像中大（畫面 480x800）：
 *   2:3  -> 480x720，上下各 40px
 *   3:4  -> 480x640，上下各 80px
 *   9:16 -> 450x800，左右各 15px
 *   橫式 -> 本來就超過門檻，行為不變 */
#define MAX_CROP_PCT    0u

/* 銳化強度，256 = 100%。
 *
 * 原本是 100。用真實照片（高對比平滑邊緣，例如翅膀對天空）放大八倍並排比較
 * 之後降到 30 —— 銳化是**唯一**在真實照片上看得出差別的變數：換濾波核
 * （盒狀 / Mitchell / SSIM 最佳化）肉眼幾乎分不出來，但銳化強度一眼就看得到。
 *
 * 原因是它同時做兩件事：補回縮小造成的變軟（想要的），以及把邊緣過渡壓得
 * 更陡（不想要的）。2.25 倍縮小後邊緣過渡只剩一兩個像素寬，壓陡的代價就是
 * 過衝亮暈與階梯感 —— 也就是使用者說的鋸齒。
 *
 * 這是感知取捨不是對錯，數字可以再調：0 最平滑但整體偏軟，60 以上邊緣開始
 * 出現可見的亮暈。開放編譯期覆寫，測試台靠它做 A/B。 */
#ifndef SHARPEN_AMOUNT
#define SHARPEN_AMOUNT  0
#endif

static JPEG_HandleTypeDef g_hjpeg;

/* 使用者要求放棄時回傳 true。未註冊就永遠不放棄。 */
static bool (*g_abort_fn)(void);

void photo_set_abort_check(bool (*fn)(void))
{
    g_abort_fn = fn;
}

static bool aborted(void)
{
    return (g_abort_fn != NULL) && g_abort_fn();
}

/* 顯示方向。預設 AUTO：依每張照片的長寬比自動選，直式照片用直立畫布、
 * 橫式照片用橫向畫布，兩種都能填滿而不是各有一半躺著。
 * 使用者仍可在選單或暫停中的控制列固定成直立／橫向。 */
static photo_orient_t g_orient = PHOTO_ORIENT_AUTO;

void photo_set_orientation(photo_orient_t o)
{
    if (o < PHOTO_ORIENT_COUNT) {
        g_orient = o;
    }
}

photo_orient_t photo_get_orientation(void)
{
    return g_orient;
}

/* 解碼器實際吐出的 YCbCr 位元組數。
 *
 * 不能在 HAL_JPEG_Decode() 返回後去讀 hjpeg.JpegOutCount —— HAL 在結束時會先
 * 用 DataReadyCallback 把長度交出來，然後立刻把計數清成 0。__weak 的預設回呼
 * 什麼都不做，長度就這樣掉了，轉色函式收到 DataCount=0 一個 MCU 都不會轉。 */
static volatile uint32_t g_out_total;

static const uint8_t *g_in_ptr;
static uint32_t       g_in_left;

/* 診斷用，SWD 讀得到。 */
volatile int32_t  g_dbg_open;
volatile uint32_t g_dbg_size;
volatile int32_t  g_dbg_read;
volatile uint32_t g_dbg_done;
volatile uint32_t g_dbg_sof;
volatile uint32_t g_dbg_outcount;
volatile uint32_t g_dbg_w, g_dbg_h, g_dbg_css;
volatile uint32_t g_dbg_ms_decode, g_dbg_ms_scale, g_dbg_ms_out;

/* 分段計時（微秒）。
 *
 * 原本只有 g_dbg_ms_decode 一個數字，而它的 t0 設在 HAL_JPEG_Decode() 之前、
 * 在 convert() 之後才結算 —— 也就是「硬體解碼 + CPU 轉色」混在一起，
 * 分不出誰是瓶頸。讀檔那段更是完全沒計時。要判斷「該不該把轉色改成 DMA2D」，
 * 就必須先能分開看這五段。
 *
 * 用 DWT 週期計數器而不是 HAL_GetTick()：後者只有 1ms 解析度，
 * 而我們要比較的階段可能只差幾毫秒。600MHz 下 32 位元約 7.16 秒才繞回，
 * 單一階段不可能跑那麼久，直接相減不必處理 wrap。 */
volatile uint32_t g_dbg_us_read;    /* SD 讀檔 */
volatile uint32_t g_dbg_us_jpeg;    /* 硬體 JPEG 解碼 */
volatile uint32_t g_dbg_us_inv;     /* 轉色前失效 YCbCr 那塊的快取 */
volatile uint32_t g_dbg_us_cc;      /* YCbCr -> RGB888 轉色（jpeg_utils，CPU）*/
volatile uint32_t g_dbg_us_scale;   /* 盒狀濾波縮放 */
volatile uint32_t g_dbg_us_out;     /* 銳化 + 抖動 + 旋轉 */
volatile uint32_t g_dbg_us_total;   /* photo_show() 全程 */

/* 單張的數字會隨照片大小跳動，看平均才準。累加後除以張數即可。
 * 只累計成功走完的那些（中途放棄的不計，否則平均會被拉低）。 */
volatile uint32_t g_dbg_nphoto;
volatile uint32_t g_dbg_sum_read, g_dbg_sum_jpeg, g_dbg_sum_inv, g_dbg_sum_cc;
volatile uint32_t g_dbg_sum_scale, g_dbg_sum_out, g_dbg_sum_total;

/* 累計來源像素數（單位：千像素，免得長時間播放會溢位）。
 *
 * 隨機播放每次抽到的照片尺寸都不同，直接比兩次執行的平均毫秒數會被樣本
 * 差異蓋過真正的改動 —— 前面已經連續三輪都得拿 JPEG 解碼時間當尺寸代理
 * 去估算，估出來的倍率從 1.7 到 2.4 都有。有了這個就能算「每百萬像素幾
 * 毫秒」，不同批次之間可以直接比。 */
volatile uint32_t g_dbg_sum_kpx;
volatile uint32_t g_dbg_need_rgb;   /* 這張照片解碼需要多少 RGB 空間 */
volatile uint32_t g_dbg_toobig;     /* 因為太大被跳過的張數 */
volatile uint32_t g_dbg_notbaseline; /* 不是 baseline 被擋下的張數 */

/* 實際的讀寫範圍。與預期值一比就知道有沒有越界，不用再靠推理。 */
volatile uint32_t g_dbg_dw, g_dbg_dh, g_dbg_ox, g_dbg_oy;
volatile uint32_t g_dbg_srcw, g_dbg_srch, g_dbg_sox, g_dbg_soy;
volatile uint32_t g_dbg_maxsy;      /* downscale 讀到的最大來源列 */
volatile uint32_t g_dbg_maxsx;      /* downscale 讀到的最大來源行 */
volatile uint32_t g_dbg_maxdst;     /* downscale 寫到的最大縮圖位移 */
volatile uint32_t g_dbg_maxfb;      /* 寫到的最大 framebuffer 索引 */
volatile uint32_t g_dbg_maxscl;     /* sharpen 讀到的最大縮圖位移 */
char              g_dbg_path[160];

/* 影片模式會把這兩個回呼接走（見 photo.h 的說明）。沒註冊時完全等於不存在，
 * 照片路徑的行為跟加這段之前逐位元組相同。 */
static void (*g_jpeg_hook_ready)(void *, uint8_t *, uint32_t);
static void (*g_jpeg_hook_get)(void *, uint32_t);

void photo_set_jpeg_hooks(void (*ready)(void *, uint8_t *, uint32_t),
                          void (*get)(void *, uint32_t))
{
    g_jpeg_hook_ready = ready;
    g_jpeg_hook_get   = get;
}

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataOut,
                                uint32_t OutDataLength)
{
    if (g_jpeg_hook_ready != NULL) {
        g_jpeg_hook_ready(hjpeg, pDataOut, OutDataLength);
        return;
    }
    /* 只記錄長度，「不要」在這裡呼叫 HAL_JPEG_ConfigOutputBuffer()。
     *
     * 這裡踩過一個很難查的坑：原本為了防止輸出緩衝區填滿時 HAL 從頭覆蓋，
     * 在回呼裡把緩衝區往後推。但 HAL 在**解碼結束時也會呼叫這個回呼**，
     * 這時又餵一塊新緩衝區給它，HAL 就當作還能繼續輸出 —— 於是反覆觸發，
     * 直到把整個緩衝區用完才停。
     *
     * 後果是連鎖的：輸出長度從 3,254,400 變成 6,291,456（剛好是緩衝區上限），
     * 轉色函式據此算出 16384 個 MCU（實際只有 8475），多轉的部分往 RGB 緩衝區
     * 後面寫，溢位落點正好是縮圖緩衝區的開頭 —— 也就是畫面最上面幾列；
     * 而真正的影像尾端從未被填入，畫面下方就是未初始化記憶體。
     * 「上緣出現別張照片、下緣一片雜訊」這兩個症狀，其實是同一個原因。
     *
     * 正確做法：一開始就把整塊緩衝區交給 HAL_JPEG_Decode()（它已經這麼做了），
     * 回呼只負責回報「這批資料寫到哪裡為止」。用結束位移取最大值而不是累加，
     * 因為 HAL 在某些路徑上會對同一批資料呼叫兩次。 */
    uint32_t end = (uint32_t)(pDataOut - YCBCR_BUF) + OutDataLength;

    if (end > g_out_total) {
        g_out_total = end;
    }
}

void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *hjpeg, uint32_t NbDecodedData)
{
    if (g_jpeg_hook_get != NULL) {
        g_jpeg_hook_get(hjpeg, NbDecodedData);
        return;
    }
    if (NbDecodedData >= g_in_left) {
        g_in_left = 0;
        return;
    }
    g_in_ptr  += NbDecodedData;
    g_in_left -= NbDecodedData;
    HAL_JPEG_ConfigInputBuffer(hjpeg, (uint8_t *)g_in_ptr, g_in_left);
}

/* HAL 的 __weak 版本什麼都不做，JPEG 的時脈不開就跑不起來。 */
void HAL_JPEG_MspInit(JPEG_HandleTypeDef *hjpeg)
{
    if (hjpeg->Instance == JPEG) {
        __HAL_RCC_JPEG_CLK_ENABLE();
    }
}

/* 定義在下面的「縮放與輸出」區段，開機時建表。 */
static void init_gamma_tables(void);

static inline uint32_t cyc_start(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t cyc_us(uint32_t t0)
{
    return (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u);
}


bool photo_init(void)
{
    /* DWT 週期計數器要自己開，reset 後預設是關的。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    init_gamma_tables();

    g_hjpeg.Instance = JPEG;
    if (HAL_JPEG_Init(&g_hjpeg) != HAL_OK) {
        return false;
    }
    JPEG_InitColorTables();
    return true;
}

/* 把整個檔案讀進 PSRAM。回傳位元組數，0 表示失敗。 */
static uint32_t read_whole_file(const char *path)
{
    FIL f;
    UINT got;
    uint32_t size, done = 0;

    strncpy(g_dbg_path, path, sizeof(g_dbg_path) - 1u);
    g_dbg_path[sizeof(g_dbg_path) - 1u] = 0;

    g_dbg_open = (int32_t)f_open(&f, path, FA_READ);
    if (g_dbg_open != FR_OK) {
        return 0;
    }
    size = (uint32_t)f_size(&f);
    g_dbg_size = size;
    if (size == 0u || size > JPEG_FILE_CAP) {
        f_close(&f);
        return 0;
    }

    /* 一次讀 64KB。FatFs 對大塊讀取會走多磁區傳輸，比逐磁區快很多。 */
    while (done < size) {
        UINT want = (UINT)((size - done) > 65536u ? 65536u : (size - done));
        g_dbg_read = (int32_t)f_read(&f, JPEG_FILE_BUF + done, want, &got);
        if (g_dbg_read != FR_OK || got == 0u) {
            g_dbg_done = done;
            f_close(&f);
            return 0;
        }
        done += got;
        g_dbg_done = done;
    }
    f_close(&f);
    return size;
}

/* 走標記鏈找出 SOF 的種類。硬體 JPEG 只吃 baseline（SOF0）。 */
static void find_sof(const uint8_t *p, uint32_t len)
{
    uint32_t i = 2;

    g_dbg_sof = 0;
    if (len < 4u || p[0] != 0xFFu || p[1] != 0xD8u) {
        return;
    }
    while (i + 4u < len) {
        uint8_t  m = p[i + 1];
        uint16_t seglen = (uint16_t)((p[i + 2] << 8) | p[i + 3]);

        if (p[i] != 0xFFu) {
            return;
        }
        if (m >= 0xC0u && m <= 0xCFu &&
            m != 0xC4u && m != 0xC8u && m != 0xCCu) {
            g_dbg_sof = m;
            return;
        }
        if (m == 0xDAu || m == 0xD9u || seglen < 2u) {
            return;
        }
        i += 2u + seglen;
    }
}

/* ------------------------------------------------------------------ */
/* 縮放與輸出                                                          */
/* ------------------------------------------------------------------ */

/* 重取樣的本質是加權平均，而平均只在**線性光**空間才有物理意義。
 * JPEG 解出來的是 sRGB —— 那是感知編碼（約 gamma 2.2），直接對編碼值取平均
 * 等於假設亮度尺度是線性的，實際上是指數的。
 *
 * 症狀正好落在我們要修的地方：誤差集中在高頻（邊緣），平坦區幾乎不受影響。
 * 亮暗交界處會偏暗，因為對編碼值平均會低估亮像素的實際光量。
 *
 * 成本很不對稱，所以兩個方向用不同做法：
 *   正向（sRGB -> 線性）每個**來源**像素一次，1200x1800 就是 216 萬次 -> 查表
 *   反向（線性 -> sRGB）每個**目的**像素一次，480x800 只有 38 萬次 -> 二分搜尋
 * 反向用搜尋就不必為了精度另外放一張大表；差一個數量級的呼叫次數，
 * 八次比較完全吃得下。 */
static uint16_t g_srgb_to_lin[256];

/* 反查表，索引 = 線性值 >> 4（4096 格，4KB，塞得進 D-Cache）。
 *
 * 一開始這裡是在正向表上做二分搜尋。正確但很貴：每個目的像素三個通道
 * 各八次帶分支的迭代，480x800 就是九百多萬次難以預測的分支。
 * 換成查表之後這些全部消失。
 *
 * 格子粒度是 16（65536/4096），而最暗處相鄰兩階 sRGB 之間的線性差距約 20
 * （階 1 對應線性 19.9），所以連最暗的幾階都還分得開，不會在暗部併階。 */
static uint8_t g_lin_to_srgb[4096];

static void init_gamma_tables(void)
{
    uint32_t i, code;

    for (i = 0; i < 256u; i++) {
        double c = (double)i / 255.0;
        /* sRGB 的轉換函式是分段的，近黑處是一段直線。用純 2.2 次方近似
         * 在暗部會有可見誤差，這裡照標準寫。只在開機時算一次。 */
        double l = (c <= 0.04045) ? (c / 12.92)
                                  : pow((c + 0.055) / 1.055, 2.4);
        g_srgb_to_lin[i] = (uint16_t)(l * 65535.0 + 0.5);
    }

    /* lin 隨 i 單調遞增，所以 code 只會往前走，整個迴圈是 O(4096+256)。 */
    code = 0;
    for (i = 0; i < 4096u; i++) {
        uint32_t lin = (i << 4) + 8u;          /* 取格子中心 */
        uint32_t c;

        while (code < 255u && (uint32_t)g_srgb_to_lin[code + 1u] <= lin) {
            code++;
        }
        c = code;
        if (c < 255u) {                        /* 跟上一階比誰近，取近的 */
            uint32_t d0 = lin - g_srgb_to_lin[c];
            uint32_t d1 = (uint32_t)g_srgb_to_lin[c + 1u] - lin;
            if (d1 < d0) { c++; }
        }
        g_lin_to_srgb[i] = (uint8_t)c;
    }
}

/* 關掉就退回「直接對 sRGB 編碼值平均」，測試台靠這個做 A/B。 */
#ifndef RESAMPLE_LINEAR
#define RESAMPLE_LINEAR 1
#endif

#if RESAMPLE_LINEAR
#define TO_LIGHT(v)    ((uint32_t)g_srgb_to_lin[(v)])
#define FROM_LIGHT(v)  g_lin_to_srgb[(v) >> 4]
#else
#define TO_LIGHT(v)    ((uint32_t)(v))
#define FROM_LIGHT(v)  ((uint8_t)(v))
#endif

static uint32_t g_dw, g_dh, g_ox, g_oy;   /* 目的地幾何（邏輯直立座標） */

/* 決定縮放比例與來源取用範圍。
 *
 * 目的地尺寸「直接指定」，不從縮放比例反算 —— 這是先前踩過的坑：
 *
 *   by_h  = (800 << 16) / 1800 = 29127.11 -> 截斷成 29127
 *   g_dh  = (1800 * 29127) >> 16 = 799.99 -> 截斷成 799
 *
 * 差 0.11 就讓高度變成 799，繪圖迴圈只寫到 y=798，最後一列永遠是清畫面留下
 * 的黑色，看起來就是底部一條黑線。填滿模式的目的地本來就是整個螢幕，不該
 * 讓它去承受兩次除法的誤差。
 *
 * 所以改成：先決定要「填滿」還是「完整顯示」，再直接寫死目的地尺寸，
 * 用目的地去反推該取用哪一塊來源。誤差只會落在來源取樣座標上，那裡差一個
 * 像素看不出來，也不會留下沒畫到的空白。
 */
static void plan_geometry(uint32_t sw, uint32_t sh,
                          uint32_t *src_w, uint32_t *src_h,
                          uint32_t *sox, uint32_t *soy)
{
    /* 畫布尺寸隨方向改變：直立 480x800、橫向 800x480。
     * 不能再用 GFX_W/GFX_H 常數，那兩個永遠是直立的值。 */
    const uint32_t cw = (uint32_t)gfx_width();
    const uint32_t ch = (uint32_t)gfx_height();

    /* 比較「來源寬高比」與「畫面寬高比」，用交叉相乘避免浮點與除法誤差。 */
    uint64_t src_ratio = (uint64_t)sw * ch;    /* sw/sh vs cw/ch */
    uint64_t dst_ratio = (uint64_t)cw * sh;
    bool     wider     = (src_ratio > dst_ratio); /* 來源比畫面更寬 */

    /* 填滿畫面要裁掉多少？用面積比估算，超過上限就退回完整顯示。 */
    uint32_t crop_pct;
    if (wider) {
        /* 以高度為準放大，裁掉左右 */
        uint32_t used_w = (uint32_t)(((uint64_t)sh * cw) / ch);
        crop_pct = 100u - (used_w * 100u) / sw;
    } else {
        /* 以寬度為準放大，裁掉上下 */
        uint32_t used_h = (uint32_t)(((uint64_t)sw * ch) / cw);
        crop_pct = 100u - (used_h * 100u) / sh;
    }

    if (crop_pct <= MAX_CROP_PCT) {
        /* 填滿：目的地就是整個畫面，來源取中間一塊同比例的區域。 */
        g_dw = cw;
        g_dh = ch;
        if (wider) {
            *src_h = sh;
            *src_w = (uint32_t)(((uint64_t)sh * cw) / ch);
        } else {
            *src_w = sw;
            *src_h = (uint32_t)(((uint64_t)sw * ch) / cw);
        }
    } else {
        /* 完整顯示：整張都要，某一邊會留黑邊。 */
        *src_w = sw;
        *src_h = sh;
        if (wider) {
            g_dw = cw;
            g_dh = (uint32_t)(((uint64_t)sh * cw) / sw);
        } else {
            g_dh = ch;
            g_dw = (uint32_t)(((uint64_t)sw * ch) / sh);
        }
    }

    if (g_dw == 0u) { g_dw = 1u; }
    if (g_dh == 0u) { g_dh = 1u; }
    if (g_dw > cw) { g_dw = cw; }
    if (g_dh > ch) { g_dh = ch; }
    if (*src_w > sw) { *src_w = sw; }
    if (*src_h > sh) { *src_h = sh; }

    *sox = (sw - *src_w) / 2u;
    *soy = (sh - *src_h) / 2u;

    g_ox = (cw - g_dw) / 2u;
    g_oy = (ch - g_dh) / 2u;
}

/* 面積平均縮放：RGB888 全尺寸 -> RGB888 縮圖（邏輯直立方向，逐列存放）。
 *
 * 每個目的像素對應到來源上一塊 sx_step x sy_step 的矩形（16.16 定點）。
 * 落在矩形邊界上的來源像素只有一部分被涵蓋，所以**按重疊面積加權**，
 * 不是整顆算進來或整顆丟掉。
 *
 * 舊版是用整數邊界切盒子（sy0 = (dy*step)>>16），來源像素非全進即全出。
 * 這在非整數倍縮放時有兩個後果，兩個都會變成看得見的鋸齒：
 *
 *   1. 盒子大小會跳動。2.25 倍時是 2,2,2,3 的週期，取樣中心跟著漂移，
 *      邊緣每四個目的像素就錯開一次 —— 規律的階梯特別刺眼。
 *   2. 每個目的像素只由 2~3 顆來源像素平均，灰階過渡只有寥寥幾級，
 *      邊緣幾乎是硬跳而不是漸層。
 *
 * PC 測試台（test/）拿 Lanczos 當參考量過平均絕對差：
 * 整數盒 4.53、面積平均 2.64，差 1.7 倍。銳化關掉數字幾乎不變（4.37），
 * 所以鋸齒是縮放造成的，不是銳化。
 *
 * 成本：內層多了每來源像素一次乘法。這一段實測是等匯流排等掉的
 * （78 週期/來源像素，算術本身只需個位數），多的乘法幾乎不花時間。
 *
 * 外層走目的 y、內層走目的 x，來源與目的都沿著列連續存取，對快取最友善。 */
/* 每個目的行的水平權重。同一組權重會被 g_dh 個目的列重複使用 —— 原本每個
 * 來源樣本都現算一次 min/max/減法/移位，480x800 各取 3.25x3.25 個樣本，
 * 就是四百萬次重複運算。先建表，內層迴圈只剩一次查表。
 *
 * 存成緊湊格式（偏移＋長度）而不是固定跨距：2.25 倍時實際摸到的資料只有
 * 約 4KB，留得住 D-Cache；固定跨距要配到 19KB 就放不下了。 */
#define MAX_TAPS  20
static uint16_t g_wx[GFX_MAX_DIM * MAX_TAPS];
static uint16_t g_wx_off[GFX_MAX_DIM];
static uint16_t g_wx_start[GFX_MAX_DIM];
static uint8_t  g_wx_n[GFX_MAX_DIM];

volatile uint32_t g_dbg_taps_clamped;   /* tap 數超過上限被截掉的次數 */

static void build_x_weights(uint32_t sx_step, uint32_t xbase, uint32_t xlimit)
{
    uint32_t off = 0;

    for (uint32_t dx = 0; dx < g_dw; dx++) {
        uint32_t fx0 = xbase + dx * sx_step;
        uint32_t fx1 = xbase + (dx + 1u) * sx_step;
        uint32_t sx0 = fx0 >> 16;
        uint32_t sx1 = (fx1 + 0xFFFFu) >> 16;   /* 進位才不會漏掉邊界那顆 */
        uint32_t n = 0;

        if (sx1 <= sx0)   { sx1 = sx0 + 1u; }
        if (sx1 > xlimit) { sx1 = xlimit; }
        /* 極端縮放比（超過約 14 倍）才可能撞到。截掉會少平均幾顆來源像素，
         * 畫質影響很小，但要留下痕跡而不是默默發生。 */
        if (sx1 - sx0 > MAX_TAPS) {
            sx1 = sx0 + MAX_TAPS;
            g_dbg_taps_clamped++;
        }

        g_wx_off[dx]   = (uint16_t)off;
        g_wx_start[dx] = (uint16_t)sx0;
        for (uint32_t xx = sx0; xx < sx1; xx++) {
            /* 這顆來源像素有多少落在涵蓋範圍內，就給多少權重。 */
            uint32_t xl = (xx << 16) > fx0 ? (xx << 16) : fx0;
            uint32_t xr = ((xx + 1u) << 16) < fx1 ? ((xx + 1u) << 16) : fx1;
            uint32_t w  = (xr > xl) ? ((xr - xl) >> 8) : 0u;
            g_wx[off++] = (uint16_t)w;
            n++;
        }
        g_wx_n[dx] = (uint8_t)n;

        /* 正規化成總和「剛好」256。
         *
         * 這樣水平方向累加出來的 sum(lin * wx) 上限是 65535*256，右移 8 位
         * 就直接是 0..65535 的加權平均 —— 內層迴圈完全不必做除法，
         * 也不必為了不同的 dx 記錄不同的除數。
         *
         * 順帶把邊界被夾掉的情況一併處理掉：那些 dx 的原始權重和本來就小於
         * 一格，正規化等於自動重新分配，不會出現邊緣變暗。 */
        {
            uint16_t *wv = &g_wx[g_wx_off[dx]];
            uint32_t  sum = 0, out = 0, big = 0;

            for (uint32_t k = 0; k < n; k++) { sum += wv[k]; }
            if (sum == 0u) { sum = 1u; }
            for (uint32_t k = 0; k < n; k++) {
                wv[k] = (uint16_t)((wv[k] * 256u + sum / 2u) / sum);
                out  += wv[k];
                if (wv[k] > wv[big]) { big = k; }
            }
            /* 四捨五入的餘數補到最大的那一項，總和才會精確是 256。 */
            if (n > 0u) {
                wv[big] = (uint16_t)((uint32_t)wv[big] + 256u - out);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 串流式重取樣                                                        */
/* ------------------------------------------------------------------ */

/* 來源列一次餵一條進來，累加到目的列，滿了就吐出去。
 *
 * 為什麼要改成這個形狀：原本的寫法是「走目的像素、回頭去抓來源」，需要
 * 一整張全尺寸 RGB888 攤在 PSRAM 裡（2.16MP 就是 6.5MB）。那塊被寫進去
 * 一次、又被完整讀出來一次，共 13MB 的來回搬運，而縮放對每個來源像素
 * 其實只用一次 —— 純粹是為了「能隨機存取」才付的代價。
 *
 * 改成串流之後只需要一條 band（一列 MCU）的緩衝區，放得進內部 SRAM，
 * 那 13MB 的 PSRAM 流量整個消失。
 *
 * 附帶的好處是水平與垂直分離了：水平重取樣現在「每條來源列做一次」，
 * 而不是「每個目的列都把它涵蓋的來源列重做一遍」。原本 3.25x3.25 的
 * 二維取樣變成 3.25 + 少數幾次加法。
 *
 * 這三個函式刻意跟「來源從哪來」解耦：韌體用 MCU band 餵，PC 測試台用
 * 整張影像逐列餵，兩邊跑的是同一段重取樣程式碼。
 */
static uint16_t g_hrow[GFX_MAX_DIM * 3];      /* 水平重取樣後的一列，線性光 0..65535 */
static uint32_t g_acc[GFX_MAX_DIM * 3];       /* 目前這個目的列的垂直累加 */
static uint32_t g_rs_dy;                /* 正在累積哪個目的列 */
static uint32_t g_rs_wsum;              /* 這個目的列已累積的垂直權重 */
static uint32_t g_rs_sy_step, g_rs_ybase, g_rs_ylimit;

static void resample_begin(uint32_t src_h, uint32_t src_w,
                           uint32_t sox, uint32_t soy)
{
    g_rs_sy_step = (src_h << 16) / g_dh;
    g_rs_ybase   = soy << 16;
    g_rs_ylimit  = soy + src_h;
    g_rs_dy      = 0;
    g_rs_wsum    = 0;

    build_x_weights((src_w << 16) / g_dw, sox << 16, sox + src_w);
    memset(g_acc, 0, (size_t)g_dw * 3u * sizeof(g_acc[0]));
}

/* 把累積好的目的列寫進縮圖緩衝區。 */
static void resample_emit(void)
{
    uint8_t  *dst = SCALED_BUF + (size_t)g_rs_dy * g_dw * 3u;
    uint32_t  w   = g_rs_wsum;
    uint32_t  half;

    if (w == 0u) { w = 1u; }        /* 除零比畫錯更糟 */
    half = w >> 1;

    for (uint32_t i = 0; i < g_dw * 3u; i++) {
        dst[i] = FROM_LIGHT((g_acc[i] + half) / w);
    }
    {
        uint32_t off = (uint32_t)(g_dw * 3u + (uint32_t)(dst - SCALED_BUF));
        if (off > g_dbg_maxdst) { g_dbg_maxdst = off; }
    }

    g_rs_dy++;
    g_rs_wsum = 0;
    memset(g_acc, 0, (size_t)g_dw * 3u * sizeof(g_acc[0]));
}

/* row 指向來源影像第 yy 列的開頭（RGB888，整張影像寬度）。
 * yy 是絕對來源列號，所以水平權重表裡的 sx0 可以直接拿來索引。 */
static void resample_row(const uint8_t *row, uint32_t yy)
{
    uint32_t ytop = yy << 16;
    uint32_t ybot = (yy + 1u) << 16;

    if (yy < (g_rs_ybase >> 16) || yy >= g_rs_ylimit || g_rs_dy >= g_dh) {
        return;
    }

    /* 水平重取樣。權重已正規化成總和 256，所以右移 8 位就是加權平均，
     * 上限 65535*256 右移之後剛好回到 0..65535，不必除法。 */
    for (uint32_t dx = 0; dx < g_dw; dx++) {
        const uint16_t *wx   = &g_wx[g_wx_off[dx]];
        const uint8_t  *p    = row + (size_t)g_wx_start[dx] * 3u;
        uint32_t        ntap = g_wx_n[dx];
        uint32_t        r = 0, g = 0, b = 0;

        for (uint32_t k = 0; k < ntap; k++) {
            uint32_t w = wx[k];
            r += TO_LIGHT(p[0]) * w;
            g += TO_LIGHT(p[1]) * w;
            b += TO_LIGHT(p[2]) * w;
            p += 3;
        }
        g_hrow[dx * 3u + 0u] = (uint16_t)(r >> 8);
        g_hrow[dx * 3u + 1u] = (uint16_t)(g >> 8);
        g_hrow[dx * 3u + 2u] = (uint16_t)(b >> 8);
    }

    if (yy > g_dbg_maxsy) { g_dbg_maxsy = yy; }

    /* 這條來源列可能橫跨不只一個目的列（縮放比不是整數時很常見），
     * 所以是迴圈不是 if。放大時一條來源列會餵飽好幾個目的列，同樣走這裡。 */
    while (g_rs_dy < g_dh) {
        uint32_t fy0 = g_rs_ybase + g_rs_dy * g_rs_sy_step;
        uint32_t fy1 = g_rs_ybase + (g_rs_dy + 1u) * g_rs_sy_step;
        uint32_t top = (fy0 > ytop) ? fy0 : ytop;
        uint32_t bot = (fy1 < ybot) ? fy1 : ybot;

        if (bot > top) {
            /* 重疊量換算成 0..256。極端放大時可能不足 1，夾成 1 免得
             * 整個目的列拿不到任何貢獻而變成黑的。 */
            uint32_t wy = (bot - top) >> 8;

            if (wy == 0u) { wy = 1u; }
            for (uint32_t i = 0; i < g_dw * 3u; i++) {
                g_acc[i] += (uint32_t)g_hrow[i] * wy;
            }
            g_rs_wsum += wy;
        }

        if (fy1 <= ybot) {
            resample_emit();        /* 這個目的列收齊了 */
        } else {
            break;                  /* 還要等後面的來源列 */
        }
    }
}

static void resample_end(void)
{
    /* 最後一個目的列可能因為來源高度被 MCU 邊界截掉而沒收齊。 */
    if (g_rs_dy < g_dh && g_rs_wsum > 0u) {
        resample_emit();
    }
}

/* 一條 MCU 列轉出來的 RGB888。**放內部 SRAM 是整個改動的重點** ——
 * band 待在 SRAM，全尺寸 RGB888 那 13MB 的 PSRAM 來回搬運就整個消失。
 *
 * 160KB 的容量：4:2:0（MCU 高 16）撐得住寬 3413 的照片，
 * 4:4:4 與 4:2:2（MCU 高 8）撐到 6826。超過就退回用 PSRAM 的 RGB_BUF，
 * 慢，但正確。
 *
 * 尾端多留 256 bytes：右邊界的部分 MCU 會多寫幾個像素，寫入上界算出來是
 * 16*ScaledWidth + 45。board-notes 11.5 那條「緩衝區之間不要首尾相接」。 */
#define BAND_CAP  (160u * 1024u)
static uint8_t g_band[BAND_CAP + 256u];

volatile uint32_t g_dbg_band_psram;   /* band 放不進 SRAM 而退回 PSRAM 的次數 */

/* 逐條 MCU 列解碼並重取樣，不需要全尺寸的 RGB888 中間層。 */
static void downscale_banded(JPEG_YCbCrToRGB_Convert_Function convert,
                             uint32_t img_w, uint32_t img_h,
                             uint32_t mcu_w, uint32_t mcu_h, uint32_t mcu_bytes,
                             uint32_t src_w, uint32_t src_h,
                             uint32_t sox, uint32_t soy)
{
    uint32_t per_row = (img_w + mcu_w - 1u) / mcu_w;
    uint32_t n_rows  = (img_h + mcu_h - 1u) / mcu_h;
    uint32_t stride  = img_w * 3u;
    size_t   need    = (size_t)stride * mcu_h + 256u;
    uint8_t *band    = g_band;
    uint32_t cc      = 0;

    if ((size_t)g_dw * g_dh * 3u > SCALED_CAP) {
        return;
    }
    if (need > sizeof(g_band)) {
        band = RGB_BUF;
        g_dbg_band_psram++;
    }

    resample_begin(src_h, src_w, sox, soy);

    for (uint32_t m = 0; m < n_rows; m++) {
        uint32_t first = m * mcu_h;
        uint32_t done  = 0;
        uint32_t c0;

        if (first >= soy + src_h)  { break; }      /* 剩下的都在取用範圍下方 */
        if (first + mcu_h <= soy)  { continue; }   /* 整條在取用範圍上方 */
        if (aborted())             { return; }

        /* BlockIndex 傳 0，同時把輸入指標滑到這條 MCU 列的起點。
         *
         * 轉色函式是用 currentMCU（從 BlockIndex 起算）去推輸出位址的
         * xRef = (currentMCU * mcu_w / WidthExtend) * mcu_h，而輸入指標
         * 則是每處理完一個 MCU 就 += 一個區塊、與索引無關。所以傳 0
         * 等於告訴它「這些 MCU 是影像最上面那一條」，輸出就落在
         * band 的第 0..mcu_h-1 列 —— 不必給它整張影像大小的緩衝區，
         * 也不必動任何指標偏移的取巧手法。 */
        c0 = cyc_start();
        (void)convert(YCBCR_BUF + (size_t)m * per_row * mcu_bytes,
                      band, 0, per_row * mcu_bytes, &done);
        cc += cyc_us(c0);

        for (uint32_t r = 0; r < mcu_h; r++) {
            uint32_t yy = first + r;

            if (yy >= img_h) { break; }            /* MCU 補齊出來的列，丟掉 */
            resample_row(band + (size_t)r * stride, yy);
        }
    }
    resample_end();

    /* 轉色現在夾在縮放裡面，但分開記帳，之前幾輪的數字才比得下去。 */
    g_dbg_us_cc = cc;
}

static inline uint8_t clamp8(int32_t v)
{
    if (v < 0)    { return 0; }
    if (v > 255)  { return 255; }
    return (uint8_t)v;
}

/* 4x4 Bayer 矩陣。量化到 RGB565 之前加一點空間雜訊，把量化誤差打散，
 * 平滑漸層就不會出現色帶。 */
static const uint8_t BAYER[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

/* 銳化 + 抖動 + 旋轉，一次寫進 framebuffer。
 *
 * gfx 的映射是 offset = (GFX_W-1-x) * PHYS_W + y，所以實體「列」由邏輯 x
 * 決定、實體「行」由邏輯 y 決定。這裡外層走 y（縮圖的列，來源連續），
 * 目的地則是跨列寫入 —— PSRAM 設成 write-through 且不做 write-allocate，
 * 跨列寫入不會產生額外的讀取流量，所以這個方向比較划算。 */
static void sharpen_dither_rotate(void)
{
    uint16_t      *fb     = gfx_framebuffer();
    uint32_t       stride = g_dw * 3u;
    const bool     land   = gfx_is_landscape();
    const uint32_t cw     = (uint32_t)gfx_width();

    for (uint32_t y = 0; y < g_dh; y++) {
        const uint8_t *row = SCALED_BUF + (size_t)y * stride;

        if ((y & 31u) == 0u && aborted()) {
            return;
        }
        const uint8_t *up  = (y > 0u)          ? row - stride : row;
        const uint8_t *dn  = (y + 1u < g_dh)   ? row + stride : row;
        uint32_t phys_col  = g_oy + y;
        /* 橫向時畫布與面板同向，一整條邏輯列就是一段連續的實體記憶體；
         * 直立時要轉 90 度，寫入位址每次遞減一整列。兩者只差在這個基底
         * 與步進，所以先算好，內層迴圈不必再判斷方向。 */
        uint32_t fb_base   = land ? (phys_col * PHYS_W + g_ox)
                                  : ((uint32_t)(cw - 1u - g_ox) * PHYS_W + phys_col);
        int32_t  fb_step   = land ? 1 : -(int32_t)PHYS_W;

        for (uint32_t x = 0; x < g_dw; x++) {
            const uint8_t *c = row + x * 3u;
            uint32_t xl = (x > 0u) ? (x - 1u) : x;
            uint32_t xr = (x + 1u < g_dw) ? (x + 1u) : x;
            const uint8_t *d = BAYER + (((y & 3u) << 2) | (x & 3u));
            uint8_t out[3];

            for (uint32_t ch = 0; ch < 3u; ch++) {
                /* 3x3 平均當成低頻，原值減掉它就是高頻，加回去即銳化。 */
                int32_t blur = (int32_t)up[xl * 3u + ch] + up[x * 3u + ch] +
                               up[xr * 3u + ch] +
                               row[xl * 3u + ch] + c[ch] + row[xr * 3u + ch] +
                               dn[xl * 3u + ch] + dn[x * 3u + ch] +
                               dn[xr * 3u + ch];
                int32_t hi = (int32_t)c[ch] - (blur / 9);

                out[ch] = clamp8((int32_t)c[ch] +
                                 (hi * SHARPEN_AMOUNT) / 256);
            }

            {
                uint32_t so = (uint32_t)((dn + xr * 3u + 2u) - SCALED_BUF);
                uint32_t fo = (uint32_t)((int32_t)fb_base + (int32_t)x * fb_step);
                if (so > g_dbg_maxscl) { g_dbg_maxscl = so; }
                if (fo > g_dbg_maxfb)  { g_dbg_maxfb  = fo; }
            }
            {
                /* 抖動幅度取各通道 1 個量化階的一半左右。 */
                int32_t r = (int32_t)out[0] + ((int32_t)*d >> 1) - 4;
                int32_t g = (int32_t)out[1] + ((int32_t)*d >> 2) - 2;
                int32_t b = (int32_t)out[2] + ((int32_t)*d >> 1) - 4;

                fb[(size_t)(uint32_t)((int32_t)fb_base + (int32_t)x * fb_step)] =
                    (uint16_t)(((uint32_t)(clamp8(r) & 0xF8u) << 8) |
                               ((uint32_t)(clamp8(g) & 0xFCu) << 3) |
                               ((uint32_t)clamp8(b) >> 3));
            }
        }
    }
}

photo_result_t photo_show(const char *path)
{
    JPEG_ConfTypeDef info;
    JPEG_YCbCrToRGB_Convert_Function convert;
    uint32_t nb_mcu = 0;
    uint32_t size, src_w, src_h, sox, soy, t0;
    uint32_t c0, call;
    uint32_t hf, vf, bs;        /* MCU 的寬、高、位元組數 */

    call = cyc_start();

    c0   = cyc_start();
    size = read_whole_file(path);
    g_dbg_us_read = cyc_us(c0);
    if (size == 0u) {
        return PHOTO_ERR_READ;
    }
    if (aborted()) {
        return PHOTO_ABORTED;
    }
    /* **先看 SOF，不是 baseline 就直接退回，不要送進硬體解碼器。**
     *
     * 硬體 JPEG 只吃 baseline（SOF0）。餵 progressive（SOF2）給它的話它不會
     * 立刻報錯，而是**卡十秒才放棄** —— 而上層還會重試一次，所以一張圖要
     * 二十秒。實測 g_last_ms = 20016，但 g_dbg_us_read 只有 7ms、
     * g_dbg_us_jpeg 只有 27ms，時間全花在等解碼器超時。
     *
     * 走一次標記鏈只要幾微秒，這一擋讓「不能解的圖」從二十秒變成瞬間。
     * find_sof 本來就在（只是原本只寫進 g_dbg_sof 當除錯用），拿來擋剛好。 */
    find_sof(JPEG_FILE_BUF, size);
    if (g_dbg_sof != 0xC0u) {
        g_dbg_notbaseline++;
        return PHOTO_ERR_DECODE;
    }

    g_out_total = 0;
    g_in_ptr    = JPEG_FILE_BUF;
    g_in_left   = size;

    t0 = HAL_GetTick();
    c0 = cyc_start();

    /* 輪詢版解碼。相簿一次只處理一張，不需要 DMA 的非同步性，
     * 少掉回呼與中斷設定，出錯的地方也少。 */
    /* 輪詢版解碼。相簿一次只處理一張，不需要 DMA 的非同步性。
     *
     * 註：DMA 版實測快 4.8 倍（141.5 -> 30.7 ms/Mpx），輸出逐位元組相同，
     * 但整合進這條路徑會在第一張之後把週邊卡死，原因未明（board-notes 16.6）。
     * 那個題目連同影片播放一起搬到獨立專案處理，這裡維持已驗證的輪詢版。 */
    if (HAL_JPEG_Decode(&g_hjpeg, JPEG_FILE_BUF, size,
                        YCBCR_BUF, YCBCR_CAP, 10000u) != HAL_OK) {
        return PHOTO_ERR_DECODE;
    }
    g_dbg_us_jpeg = cyc_us(c0);

    if (HAL_JPEG_GetInfo(&g_hjpeg, &info) != HAL_OK) {
        return PHOTO_ERR_DECODE;
    }
    if (info.ImageWidth == 0u || info.ImageHeight == 0u) {
        return PHOTO_ERR_DECODE;
    }

    /* 容量與長度都要照「實際的色度取樣」計算 —— 之前把 css==0 當成 4:2:0，
     * 但 HAL 的定義是 0=4:4:4、1=4:2:0、2=4:2:2。每種的 MCU 尺寸與資料量：
     *   4:4:4  MCU 8x8   192 bytes（每像素 3.0）
     *   4:2:2  MCU 16x8  256 bytes（每像素 2.0）
     *   4:2:0  MCU 16x16 384 bytes（每像素 1.5）
     * 高度也要補齊到 MCU 邊界，解碼器會多寫到補齊的部分。 */
    {
        uint32_t nmcu, need_ycbcr, need_rgb, aligned_h;

        if (info.ChromaSubsampling == JPEG_420_SUBSAMPLING) {
            hf = 16u; vf = 16u; bs = 384u;
        } else if (info.ChromaSubsampling == JPEG_422_SUBSAMPLING) {
            hf = 16u; vf = 8u;  bs = 256u;
        } else {                            /* JPEG_444_SUBSAMPLING = 0 */
            hf = 8u;  vf = 8u;  bs = 192u;
        }

        nmcu = (((uint32_t)info.ImageWidth  + hf - 1u) / hf) *
               (((uint32_t)info.ImageHeight + vf - 1u) / vf);
        need_ycbcr = nmcu * bs;
        aligned_h  = (((uint32_t)info.ImageHeight + vf - 1u) / vf) * vf;
        need_rgb   = (uint32_t)info.ImageWidth * aligned_h * 3u;

        g_dbg_need_rgb = need_rgb;
        /* need_rgb 現在只在 band 塞不進 SRAM、退回用 RGB_BUF 時才用得到，
         * 但那條退路仍需要這麼多空間，所以檢查照舊。 */
        if (need_ycbcr > YCBCR_CAP || need_rgb > RGB_CAP) {
            g_dbg_toobig++;
            return PHOTO_ERR_TOO_BIG;
        }

        /* 解碼器可能多吐（HAL 對同一批資料重複回報），夾回實際需要的長度，
         * 免得轉換函式多轉出去。 */
        if (g_out_total > need_ycbcr) {
            g_out_total = need_ycbcr;
        }
    }

    /* JPEG 硬體繞過 D-Cache 直接寫 PSRAM，接下來要用 CPU 讀這塊，
     * 不先失效快取會讀到上一張的殘留內容。
     *
     * 這段單獨計時：失效是逐條快取線做的，6.5MB 要跑二十萬次迴圈，不是零成本。
     * 而且如果轉色改由 DMA2D 做，讀寫兩端都繞過快取，這段就整個不需要了 ——
     * 屬於「改用硬體」能省下來的一部分，不該混進轉色的數字裡。 */
    c0 = cyc_start();
    SCB_InvalidateDCache_by_Addr((uint32_t *)YCBCR_BUF, (int32_t)g_out_total);
    g_dbg_us_inv = cyc_us(c0);

    if (JPEG_GetDecodeColorConvertFunc(&info, &convert, &nb_mcu) != HAL_OK) {
        return PHOTO_ERR_DECODE;
    }
    g_dbg_outcount  = g_out_total;
    g_dbg_w         = info.ImageWidth;
    g_dbg_h         = info.ImageHeight;
    g_dbg_css       = info.ChromaSubsampling;
    g_dbg_ms_decode = HAL_GetTick() - t0;

    g_dbg_maxsy = 0; g_dbg_maxsx = 0; g_dbg_maxdst = 0;
    g_dbg_maxfb = 0; g_dbg_maxscl = 0;

    if (aborted()) {
        return PHOTO_ABORTED;
    }

    /* 方向要在 plan_geometry 之前決定 —— 畫布尺寸是它的輸入。
     * AUTO 就看這張照片本身是橫的還是直的。 */
    gfx_set_orientation(g_orient == PHOTO_ORIENT_LANDSCAPE ||
                        (g_orient == PHOTO_ORIENT_AUTO &&
                         info.ImageWidth > info.ImageHeight));

    plan_geometry(info.ImageWidth, info.ImageHeight,
                  &src_w, &src_h, &sox, &soy);

    g_dbg_dw = g_dw; g_dbg_dh = g_dh; g_dbg_ox = g_ox; g_dbg_oy = g_oy;
    g_dbg_srcw = src_w; g_dbg_srch = src_h;
    g_dbg_sox = sox; g_dbg_soy = soy;

    /* 照片放不滿時邊框才乾淨。 */
    gfx_clear(0x0000);

    t0 = HAL_GetTick();
    c0 = cyc_start();
    downscale_banded(convert, info.ImageWidth, info.ImageHeight,
                     hf, vf, bs, src_w, src_h, sox, soy);
    g_dbg_us_scale = cyc_us(c0);
    g_dbg_ms_scale = HAL_GetTick() - t0;

    if (aborted()) {
        return PHOTO_ABORTED;
    }

    t0 = HAL_GetTick();
    c0 = cyc_start();
    sharpen_dither_rotate();
    g_dbg_us_out = cyc_us(c0);
    g_dbg_ms_out = HAL_GetTick() - t0;

    if (aborted()) {
        return PHOTO_ABORTED;
    }

    /* 只累計完整走完的張數，中途放棄的不計 —— 否則平均會被沒跑完的拉低。 */
    g_dbg_us_total   = cyc_us(call);
    g_dbg_sum_read  += g_dbg_us_read;
    g_dbg_sum_jpeg  += g_dbg_us_jpeg;
    g_dbg_sum_inv   += g_dbg_us_inv;
    g_dbg_sum_cc    += g_dbg_us_cc;
    g_dbg_sum_scale += g_dbg_us_scale;
    g_dbg_sum_out   += g_dbg_us_out;
    g_dbg_sum_total += g_dbg_us_total;
    g_dbg_sum_kpx   += ((uint32_t)info.ImageWidth * info.ImageHeight) / 1000u;
    g_dbg_nphoto++;

    return PHOTO_OK;
}
