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

#include <string.h>

/* PSRAM 配置。彼此不重疊，最後一塊之後是 album_main.c 的播放清單。 */
#define JPEG_FILE_BUF   ((uint8_t *)0x90400000u)
#define JPEG_FILE_CAP   (4u * 1024u * 1024u)
#define YCBCR_BUF       ((uint8_t *)0x90800000u)
#define YCBCR_CAP       (8u * 1024u * 1024u)
#define RGB_BUF         ((uint8_t *)0x91000000u)      /* RGB888 全尺寸 */
#define RGB_CAP_PIXELS  (4u * 1024u * 1024u)          /* 12MB / 3 bytes */
#define SCALED_BUF      ((uint8_t *)0x91C00000u)      /* RGB888 縮圖 */

/* 填滿畫面時容許裁掉的最大比例，超過就退回完整顯示（留黑邊）。
 * 2:3 的直式照片放到 3:5 的畫面只需裁 10%；設 25 足以擋掉全景照那種
 * 會被砍掉一半的情況。 */
#define MAX_CROP_PCT    25u

/* 銳化強度，256 = 100%。縮小 2 倍以上時 0.4 左右不會有明顯光暈。 */
#define SHARPEN_AMOUNT  100

static JPEG_HandleTypeDef g_hjpeg;

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
char              g_dbg_path[160];

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataOut,
                                uint32_t OutDataLength)
{
    /* 用「這批資料的結束位移」而不是累加：HAL 在某些路徑上會對同一批資料
     * 呼叫兩次，累加會得到剛好兩倍的長度。 */
    uint32_t end = (uint32_t)(pDataOut - YCBCR_BUF) + OutDataLength;

    if (end > g_out_total) {
        g_out_total = end;
    }
    if (g_out_total + 4u < YCBCR_CAP) {
        HAL_JPEG_ConfigOutputBuffer(hjpeg, YCBCR_BUF + g_out_total,
                                    YCBCR_CAP - g_out_total);
    }
}

void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *hjpeg, uint32_t NbDecodedData)
{
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

bool photo_init(void)
{
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

static uint32_t g_dw, g_dh, g_ox, g_oy;   /* 目的地幾何（邏輯直立座標） */

/* 決定縮放比例與來源取用範圍。 */
static void plan_geometry(uint32_t sw, uint32_t sh,
                          uint32_t *src_w, uint32_t *src_h,
                          uint32_t *sox, uint32_t *soy)
{
    uint32_t by_w    = (GFX_W * 65536u) / sw;
    uint32_t by_h    = (GFX_H * 65536u) / sh;
    uint32_t contain = (by_w < by_h) ? by_w : by_h;
    uint32_t cover   = (by_w > by_h) ? by_w : by_h;
    uint32_t crop    = 100u - (contain * 100u) / cover;
    uint32_t scale   = (crop <= MAX_CROP_PCT) ? cover : contain;

    g_dw = (sw * scale) >> 16;
    g_dh = (sh * scale) >> 16;
    if (g_dw == 0u) { g_dw = 1u; }
    if (g_dh == 0u) { g_dh = 1u; }
    if (g_dw > GFX_W) { g_dw = GFX_W; }
    if (g_dh > GFX_H) { g_dh = GFX_H; }

    /* 實際用到的來源範圍。cover 時小於整張照片，差額從中間等量裁掉。 */
    *src_w = (g_dw << 16) / scale;
    *src_h = (g_dh << 16) / scale;
    if (*src_w > sw) { *src_w = sw; }
    if (*src_h > sh) { *src_h = sh; }
    *sox = (sw - *src_w) / 2u;
    *soy = (sh - *src_h) / 2u;

    g_ox = (GFX_W - g_dw) / 2u;
    g_oy = (GFX_H - g_dh) / 2u;
}

/* 盒狀濾波縮放：RGB888 全尺寸 -> RGB888 縮圖（邏輯直立方向，逐列存放）。
 *
 * 外層走目的 y、內層走目的 x，這樣來源與目的都是沿著列連續存取，
 * 對快取最友善。 */
static void downscale(const uint8_t *src, uint32_t sw,
                      uint32_t src_w, uint32_t src_h,
                      uint32_t sox, uint32_t soy)
{
    uint32_t sx_step = (src_w << 16) / g_dw;
    uint32_t sy_step = (src_h << 16) / g_dh;

    for (uint32_t dy = 0; dy < g_dh; dy++) {
        uint32_t sy0 = soy + ((dy * sy_step) >> 16);
        uint32_t sy1 = soy + (((dy + 1u) * sy_step) >> 16);
        uint8_t *dst = SCALED_BUF + (size_t)dy * g_dw * 3u;

        if (sy1 <= sy0) { sy1 = sy0 + 1u; }
        if (sy1 > soy + src_h) { sy1 = soy + src_h; }

        for (uint32_t dx = 0; dx < g_dw; dx++) {
            uint32_t sx0 = sox + ((dx * sx_step) >> 16);
            uint32_t sx1 = sox + (((dx + 1u) * sx_step) >> 16);
            uint32_t r = 0, g = 0, b = 0, n = 0;

            if (sx1 <= sx0) { sx1 = sx0 + 1u; }
            if (sx1 > sox + src_w) { sx1 = sox + src_w; }

            for (uint32_t yy = sy0; yy < sy1; yy++) {
                const uint8_t *p = src + ((size_t)yy * sw + sx0) * 3u;
                for (uint32_t xx = sx0; xx < sx1; xx++) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    p += 3;
                    n++;
                }
            }

            dst[0] = (uint8_t)(r / n);
            dst[1] = (uint8_t)(g / n);
            dst[2] = (uint8_t)(b / n);
            dst += 3;
        }
    }
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
    uint16_t *fb = gfx_framebuffer();
    uint32_t  stride = g_dw * 3u;

    for (uint32_t y = 0; y < g_dh; y++) {
        const uint8_t *row = SCALED_BUF + (size_t)y * stride;
        const uint8_t *up  = (y > 0u)          ? row - stride : row;
        const uint8_t *dn  = (y + 1u < g_dh)   ? row + stride : row;
        uint32_t phys_col  = g_oy + y;

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
                /* 抖動幅度取各通道 1 個量化階的一半左右。 */
                int32_t r = (int32_t)out[0] + ((int32_t)*d >> 1) - 4;
                int32_t g = (int32_t)out[1] + ((int32_t)*d >> 2) - 2;
                int32_t b = (int32_t)out[2] + ((int32_t)*d >> 1) - 4;

                fb[(size_t)(GFX_W - 1u - (g_ox + x)) * PHYS_W + phys_col] =
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
    uint32_t nb_mcu = 0, converted = 0;
    uint32_t size, src_w, src_h, sox, soy, t0;

    size = read_whole_file(path);
    if (size == 0u) {
        return PHOTO_ERR_READ;
    }
    find_sof(JPEG_FILE_BUF, size);

    g_out_total = 0;
    g_in_ptr    = JPEG_FILE_BUF;
    g_in_left   = size;

    t0 = HAL_GetTick();

    /* 輪詢版解碼。相簿一次只處理一張，不需要 DMA 的非同步性，
     * 少掉回呼與中斷設定，出錯的地方也少。 */
    if (HAL_JPEG_Decode(&g_hjpeg, JPEG_FILE_BUF, size,
                        YCBCR_BUF, YCBCR_CAP, 10000u) != HAL_OK) {
        return PHOTO_ERR_DECODE;
    }
    if (HAL_JPEG_GetInfo(&g_hjpeg, &info) != HAL_OK) {
        return PHOTO_ERR_DECODE;
    }
    if (info.ImageWidth == 0u || info.ImageHeight == 0u ||
        (uint32_t)info.ImageWidth * info.ImageHeight > RGB_CAP_PIXELS) {
        return PHOTO_ERR_TOO_BIG;
    }
    if (g_out_total == 0u) {
        return PHOTO_ERR_DECODE;
    }

    /* 夾在這張影像實際需要的長度內。多餵一個位元組，轉換函式就會多轉一個
     * MCU，往 RGB 緩衝區後面寫出去。 */
    if (info.ChromaSubsampling == JPEG_420_SUBSAMPLING) {
        uint32_t need = (((uint32_t)info.ImageWidth + 15u) / 16u) *
                        (((uint32_t)info.ImageHeight + 15u) / 16u) * 384u;
        if (g_out_total > need) {
            g_out_total = need;
        }
    }

    /* JPEG 硬體繞過 D-Cache 直接寫 PSRAM，接下來要用 CPU 讀這塊，
     * 不先失效快取會讀到上一張的殘留內容。 */
    SCB_InvalidateDCache_by_Addr((uint32_t *)YCBCR_BUF, (int32_t)g_out_total);

    if (JPEG_GetDecodeColorConvertFunc(&info, &convert, &nb_mcu) != HAL_OK) {
        return PHOTO_ERR_DECODE;
    }
    (void)convert(YCBCR_BUF, RGB_BUF, 0, g_out_total, &converted);

    g_dbg_outcount  = g_out_total;
    g_dbg_w         = info.ImageWidth;
    g_dbg_h         = info.ImageHeight;
    g_dbg_css       = info.ChromaSubsampling;
    g_dbg_ms_decode = HAL_GetTick() - t0;

    plan_geometry(info.ImageWidth, info.ImageHeight,
                  &src_w, &src_h, &sox, &soy);

    /* 照片放不滿時邊框才乾淨。 */
    gfx_clear(0x0000);

    t0 = HAL_GetTick();
    downscale(RGB_BUF, info.ImageWidth, src_w, src_h, sox, soy);
    g_dbg_ms_scale = HAL_GetTick() - t0;

    t0 = HAL_GetTick();
    sharpen_dither_rotate();
    g_dbg_ms_out = HAL_GetTick() - t0;

    return PHOTO_OK;
}
