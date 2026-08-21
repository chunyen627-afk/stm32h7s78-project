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
#define JPEG_FILE_BUF   ((uint8_t *)0x90400000u)
#define JPEG_FILE_CAP   (2u * 1024u * 1024u)
#define YCBCR_BUF       ((uint8_t *)0x90600000u)
#define YCBCR_CAP       (10u * 1024u * 1024u)
#define RGB_BUF         ((uint8_t *)0x91000000u)      /* RGB888 全尺寸 */
#define RGB_CAP         (10u * 1024u * 1024u)
#define SCALED_BUF      ((uint8_t *)0x91A00000u)      /* RGB888 縮圖 */
#define SCALED_CAP      (2u * 1024u * 1024u)

/* 填滿畫面時容許裁掉的最大比例，超過就退回完整顯示（留黑邊）。
 * 2:3 的直式照片放到 3:5 的畫面只需裁 10%；設 25 足以擋掉全景照那種
 * 會被砍掉一半的情況。 */
#define MAX_CROP_PCT    25u

/* 銳化強度，256 = 100%。縮小 2 倍以上時 0.4 左右不會有明顯光暈。 */
#define SHARPEN_AMOUNT  100

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
volatile uint32_t g_dbg_need_rgb;   /* 這張照片解碼需要多少 RGB 空間 */
volatile uint32_t g_dbg_toobig;     /* 因為太大被跳過的張數 */

/* 實際的讀寫範圍。與預期值一比就知道有沒有越界，不用再靠推理。 */
volatile uint32_t g_dbg_dw, g_dbg_dh, g_dbg_ox, g_dbg_oy;
volatile uint32_t g_dbg_srcw, g_dbg_srch, g_dbg_sox, g_dbg_soy;
volatile uint32_t g_dbg_maxsy;      /* downscale 讀到的最大來源列 */
volatile uint32_t g_dbg_maxsx;      /* downscale 讀到的最大來源行 */
volatile uint32_t g_dbg_maxdst;     /* downscale 寫到的最大縮圖位移 */
volatile uint32_t g_dbg_maxfb;      /* 寫到的最大 framebuffer 索引 */
volatile uint32_t g_dbg_maxscl;     /* sharpen 讀到的最大縮圖位移 */
char              g_dbg_path[160];

void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataOut,
                                uint32_t OutDataLength)
{
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

    (void)hjpeg;
    if (end > g_out_total) {
        g_out_total = end;
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
    /* 比較「來源寬高比」與「畫面寬高比」，用交叉相乘避免浮點與除法誤差。 */
    uint64_t src_ratio = (uint64_t)sw * GFX_H;    /* sw/sh vs GFX_W/GFX_H */
    uint64_t dst_ratio = (uint64_t)GFX_W * sh;
    bool     wider     = (src_ratio > dst_ratio); /* 來源比畫面更寬 */

    /* 填滿畫面要裁掉多少？用面積比估算，超過上限就退回完整顯示。 */
    uint32_t crop_pct;
    if (wider) {
        /* 以高度為準放大，裁掉左右 */
        uint32_t used_w = (uint32_t)(((uint64_t)sh * GFX_W) / GFX_H);
        crop_pct = 100u - (used_w * 100u) / sw;
    } else {
        /* 以寬度為準放大，裁掉上下 */
        uint32_t used_h = (uint32_t)(((uint64_t)sw * GFX_H) / GFX_W);
        crop_pct = 100u - (used_h * 100u) / sh;
    }

    if (crop_pct <= MAX_CROP_PCT) {
        /* 填滿：目的地就是整個畫面，來源取中間一塊同比例的區域。 */
        g_dw = GFX_W;
        g_dh = GFX_H;
        if (wider) {
            *src_h = sh;
            *src_w = (uint32_t)(((uint64_t)sh * GFX_W) / GFX_H);
        } else {
            *src_w = sw;
            *src_h = (uint32_t)(((uint64_t)sw * GFX_H) / GFX_W);
        }
    } else {
        /* 完整顯示：整張都要，某一邊會留黑邊。 */
        *src_w = sw;
        *src_h = sh;
        if (wider) {
            g_dw = GFX_W;
            g_dh = (uint32_t)(((uint64_t)sh * GFX_W) / sw);
        } else {
            g_dh = GFX_H;
            g_dw = (uint32_t)(((uint64_t)sw * GFX_H) / sh);
        }
    }

    if (g_dw == 0u) { g_dw = 1u; }
    if (g_dh == 0u) { g_dh = 1u; }
    if (g_dw > GFX_W) { g_dw = GFX_W; }
    if (g_dh > GFX_H) { g_dh = GFX_H; }
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

    /* 縮圖最大就是整個畫面，理論上不會超過；真的超過寧可不畫也不要踩過界。 */
    if ((size_t)g_dw * g_dh * 3u > SCALED_CAP) {
        return;
    }

    for (uint32_t dy = 0; dy < g_dh; dy++) {
        uint32_t sy0 = soy + ((dy * sy_step) >> 16);

        /* 每 32 列給一次放棄的機會，把最壞反應時間壓到幾十毫秒。 */
        if ((dy & 31u) == 0u && aborted()) {
            return;
        }
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

            if (sy1 - 1u > g_dbg_maxsy) { g_dbg_maxsy = sy1 - 1u; }
            if (sx1 - 1u > g_dbg_maxsx) { g_dbg_maxsx = sx1 - 1u; }

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
            {
                uint32_t off = (uint32_t)(dst - SCALED_BUF);
                if (off > g_dbg_maxdst) { g_dbg_maxdst = off; }
            }
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

        if ((y & 31u) == 0u && aborted()) {
            return;
        }
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
                uint32_t so = (uint32_t)((dn + xr * 3u + 2u) - SCALED_BUF);
                uint32_t fo = (uint32_t)(GFX_W - 1u - (g_ox + x)) * PHYS_W
                              + phys_col;
                if (so > g_dbg_maxscl) { g_dbg_maxscl = so; }
                if (fo > g_dbg_maxfb)  { g_dbg_maxfb  = fo; }
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
    if (aborted()) {
        return PHOTO_ABORTED;
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
        uint32_t hf, vf, bs;
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

    g_dbg_maxsy = 0; g_dbg_maxsx = 0; g_dbg_maxdst = 0;
    g_dbg_maxfb = 0; g_dbg_maxscl = 0;

    if (aborted()) {
        return PHOTO_ABORTED;
    }

    plan_geometry(info.ImageWidth, info.ImageHeight,
                  &src_w, &src_h, &sox, &soy);

    g_dbg_dw = g_dw; g_dbg_dh = g_dh; g_dbg_ox = g_ox; g_dbg_oy = g_oy;
    g_dbg_srcw = src_w; g_dbg_srch = src_h;
    g_dbg_sox = sox; g_dbg_soy = soy;

    /* 照片放不滿時邊框才乾淨。 */
    gfx_clear(0x0000);

    t0 = HAL_GetTick();
    downscale(RGB_BUF, info.ImageWidth, src_w, src_h, sox, soy);
    g_dbg_ms_scale = HAL_GetTick() - t0;

    if (aborted()) {
        return PHOTO_ABORTED;
    }

    t0 = HAL_GetTick();
    sharpen_dither_rotate();
    g_dbg_ms_out = HAL_GetTick() - t0;

    return aborted() ? PHOTO_ABORTED : PHOTO_OK;
}
