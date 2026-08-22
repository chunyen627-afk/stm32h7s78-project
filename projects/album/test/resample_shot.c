/**
 * 在 PC 上跑相簿的縮放與後製，把結果存成 framebuffer 原始檔。
 *
 * 為什麼要有這個：畫質問題（鋸齒、色帶、光暈）只能用眼睛判斷，而在板子上
 * 改一次要重編、重燒、還要手動操作選單才看得到一張照片。這裡把「解碼之後」
 * 的整段管線搬到 PC，一次可以跑幾十種參數組合並排比較。
 *
 * 關鍵：直接 #include photo.c 本體，不重寫演算法。重寫一份「應該一樣」的
 * 程式碼來測，測到的就不是板子上實際跑的東西 —— board-notes 第七章那個
 * 「純數學測試通過、畫面卻整個上下顛倒」的教訓就是這樣來的。
 *
 * 用法：
 *   resample_shot <in.rgb> <寬> <高> <out.fb>
 * 輸入是 RGB888 逐列存放，輸出是 800x480 RGB565 實體橫向 framebuffer。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "main.h"     /* HAL 替身，要在用到 CoreDebug_Type_Stub 之前 */
#include "gfx.h"

/* photo.c 的緩衝區位址從這個基底算起。指到 malloc 出來的 32MB，
 * 版面配置與板子上完全一致，位移不用改。 */
static uint8_t *g_psram;
#define PSRAM_BASE ((uintptr_t)g_psram)

/* main.h 替身裡宣告的全域，在這裡給實體。 */
static CoreDebug_Type_Stub g_coredebug;
static DWT_Type_Stub       g_dwt;
CoreDebug_Type_Stub *CoreDebug     = &g_coredebug;
DWT_Type_Stub       *DWT           = &g_dwt;
uint32_t             SystemCoreClock = 600000000u;

/* 被測物本體。 */
#include "photo.c"

#define PSRAM_SIZE  (32u * 1024u * 1024u)

int main(int argc, char **argv)
{
    uint32_t sw, sh, src_w, src_h, sox, soy;
    size_t   in_bytes, got;
    FILE    *f;
    uint16_t *fb;

    if (argc != 5) {
        fprintf(stderr, "用法：%s <in.rgb> <寬> <高> <out.fb>\n", argv[0]);
        return 2;
    }
    sw = (uint32_t)strtoul(argv[2], NULL, 10);
    sh = (uint32_t)strtoul(argv[3], NULL, 10);
    in_bytes = (size_t)sw * sh * 3u;

    g_psram = calloc(1, PSRAM_SIZE);
    if (g_psram == NULL) {
        fprintf(stderr, "配置 32MB 失敗\n");
        return 1;
    }

    /* 來源影像放進 RGB888 全尺寸緩衝區，就是解碼＋轉色之後的狀態。 */
    if ((size_t)sw * sh * 3u > RGB_CAP) {
        fprintf(stderr, "%ux%u 超過 RGB 緩衝區容量\n", sw, sh);
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) { perror(argv[1]); return 1; }
    got = fread(RGB_BUF, 1, in_bytes, f);
    fclose(f);
    if (got != in_bytes) {
        fprintf(stderr, "%s：預期 %zu bytes，讀到 %zu\n", argv[1], in_bytes, got);
        return 1;
    }

    /* framebuffer 借用 PSRAM 開頭那塊，跟板子上的 FB0 同一個位置。 */
    fb = (uint16_t *)g_psram;
    gfx_set_framebuffer(fb);

    /* 板子上這是 photo_init() 做的。測試台直接呼叫管線中段，
     * 漏掉這行的話 sRGB 對照表全是 0，反查一律回傳 255，整張變白。 */
    init_gamma_tables();

    plan_geometry(sw, sh, &src_w, &src_h, &sox, &soy);
    gfx_clear(0x0000);

    /* 在 PC 上量相對成本。絕對值跟 M7 不能比（快取、時脈、記憶體都不同），
     * 但「改了之後快幾倍」這個比例是可以拿來迭代的 —— 每次都燒上板要花
     * 好幾分鐘還得手動操作選單，用這個先篩掉沒效果的做法。
     * clock() 只有毫秒級解析度，單輪跑出來是個位數毫秒、量不準，
     * 所以跑 20 輪取最小值（避開作業系統排程造成的雜訊）並印到小數點後兩位。 */
    {
        double best_ds = 1e9, best_out = 1e9;
        for (int i = 0; i < 20; i++) {
            clock_t t0 = clock();
            downscale(RGB_BUF, sw, src_w, src_h, sox, soy);
            clock_t t1 = clock();
            sharpen_dither_rotate();
            clock_t t2 = clock();
            double ds  = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;
            double out = (double)(t2 - t1) * 1000.0 / CLOCKS_PER_SEC;
            if (ds  < best_ds)  { best_ds  = ds; }
            if (out < best_out) { best_out = out; }
        }
        fprintf(stderr, "  縮放 %6.2f ms、輸出 %6.2f ms   (%ux%u -> %ux%u)\n",
                best_ds, best_out, src_w, src_h, g_dw, g_dh);
    }

    f = fopen(argv[4], "wb");
    if (f == NULL) { perror(argv[4]); return 1; }
    fwrite(fb, 1, (size_t)PHYS_W * PHYS_H * 2u, f);
    fclose(f);
    return 0;
}
