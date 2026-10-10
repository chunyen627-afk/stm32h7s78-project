/**
 * 賽車儀表：SimHub 經 ST-LINK 的虛擬 COM 埠（UART4）送遙測，畫在 5 吋螢幕上。
 *
 * 畫面整個是原作（caa1211/esp32-gt7-dashboard，MIT）：資料處理、七個主題、
 * 觸控選單都是原作的程式碼，一行不改，畫在 320x240 的畫布上（gt7dash/）。
 * 這裡只負責：收序列埠的行交給原作、跑原作的主迴圈、把 2 倍抗鋸齒的畫布
 * （640x480，gt7dash/hires/）貼到 800x480 面板正中央（左右各留 80 像素黑邊），
 * 以及把觸控座標換算回 320x240。
 *
 * 螢幕初始化、雙緩衝、垂直消隱換頁照搬 projects/tetris 驗證過的做法。
 */
#include "main.h"
#include "stm32h7s78_discovery.h"
#include "stm32h7s78_discovery_lcd.h"
#include "stm32h7s78_discovery_ts.h"

#include "serial.h"
#include "gt7dash.h"

#include <stdio.h>
#include <string.h>

#define PHYS_W     800
#define PHYS_H     480
#define FB0_ADDR   LCD_LAYER_0_ADDRESS   /* 0x90000000，PSRAM */
#define FB1_ADDR   LCD_LAYER_1_ADDRESS   /* 0x90200000 */
#define SCALE      2                     /* 320x240 -> 640x480 */
#define OFF_X      ((PHYS_W - GT7DASH_W * SCALE) / 2)

#define SERIAL_BAUD   115200u

/* 狀態放 DTCM，重置不會洗掉，用 SWD 讀得到。
 * [0] "RDSH"  [1] tick  [2] 收到的位元組  [3] 原作判為有效的行  [4] 無效的行
 * [5] 畫了幾格  [6] 這格貼了幾列  [7] 一格多少 CPU 週期（原作 + 貼圖）
 * [14] 其中貼圖的週期  [16] 開機以來最慢的一格（寫 0 重新量）  [8] 序列埠溢位
 * [17] 展示模式的資料間隔（毫秒，0 = 100；16 = 一秒 60 次）
 * [18] / [19] 掉格數 / 換頁次數（各寫 0 重新數；掉格 = 跟上一格隔超過 20.8ms）
 * [20] 寫 "LOWR" 再重置就不用高解析度（量原作本身的時間）
 * [9] 太長被丟的行  [10] 堆積用了多少（PSRAM）
 * [11] 寫 "DEMO" 就由板子自己產生跑圈資料（不用遊戲、不佔 COM 埠）
 * [12] 寫 0x54480000 | 主題（0~6）直接切主題（截圖驗證用）
 * [13] 寫 "NOHI" 改貼原本 320x240 的放大（跟抗鋸齒版比對用）
 * [15] 寫 "FROZ" 停止貼圖，讀 framebuffer 截圖時用 */
#define BOX ((volatile uint32_t *)0x20004020u)
#define DEMO_MAGIC  0x44454D4Fu
#define FROZ_MAGIC  0x5A4F5246u
#define LOWR_MAGIC  0x4C4F5752u   /* BOX[20]："LOWR" 重置後不用高解析度 */
#define NOHI_MAGIC  0x4E4F4849u   /* BOX[13]："NOHI" 貼原本 320x240 的放大，不用抗鋸齒版 */

static uint32_t g_front;

/* ------------------------------------------------------------------ */
/* 堆積放 PSRAM                                                         */
/* ------------------------------------------------------------------ */
/* 原作會開一張 320x240 的主畫布（150KB）加好幾張小畫布，還有大量 String，
 * 內部 SRAM 不夠放。PSRAM 前 4MB 是兩塊 framebuffer，之後的空間給堆積。
 * newlib 的 malloc 透過 _sbrk 要記憶體；定義在這裡會蓋過 libnosys 的版本。 */
#define HEAP_START  0x90400000u
#define HEAP_END    0x91000000u

void *_sbrk(ptrdiff_t incr)
{
    static uint8_t *brk = (uint8_t *)HEAP_START;
    uint8_t        *prev = brk;

    if ((uint32_t)(brk + incr) > HEAP_END) { return (void *)-1; }
    brk += incr;
    BOX[10] = (uint32_t)(brk - (uint8_t *)HEAP_START);
    return prev;
}

/* ------------------------------------------------------------------ */
/* 螢幕                                                                 */
/* ------------------------------------------------------------------ */
static void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* framebuffer 設成 write-through：CPU 寫的東西要立刻落到 PSRAM，LTDC 繞過
 * 快取直接讀（tetris 的 game_main.c 有完整說明）。只蓋前 4MB，堆積不受影響。 */
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
}

static uint16_t *back_buffer(void)
{
    return (uint16_t *)(g_front ? FB0_ADDR : FB1_ADDR);
}

static void present(void)
{
    LTDC_Layer1->CFBAR = g_front ? FB0_ADDR : FB1_ADDR;
    LTDC->SRCR = LTDC_SRCR_VBR;
    for (uint32_t guard = 0; guard < 2000000u; guard++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) { break; }
    }
    g_front ^= 1u;
}

/* 畫布貼到面板正中央。畫布是 LovyanGFX 的 swap565（高位元組在前），面板要的是
 * 一般 RGB565，所以每個像素要對調位元組。
 *
 * 平常貼 2 倍抗鋸齒的那張（640x480，REV16 一次對調兩個像素），而且只貼改過的列：
 * 雙緩衝的後台那張停在兩格之前，所以要補上「上一格和這一格」改過的列。
 * 開機、切換貼法之後的兩格整張貼。BOX[13] 寫 "NOHI" 改貼原本 320x240 的放大
 * （一個像素變成橫向兩個，一次寫 32 位元，兩列共用同一份），比對用。 */
/* DMA2D 把 2 倍畫布的 rows 列搬到 framebuffer，順便對調位元組（OPFCCR.SB）。
 * framebuffer 是 write-through，CPU 一個字一個字寫進 PSRAM 很慢（Radar 轉速錶
 * 一格要貼 350 列，CPU 要 7.5ms）；DMA2D 成批搬。來源在可快取的 PSRAM，
 * 啟動前先把 D-Cache 寫回，不然 DMA2D 會讀到還沒寫回的舊資料。 */
static void dma2d_copy_swap(const uint16_t *src, uint16_t *dst, int rows)
{
    const int w = GT7DASH_W * SCALE;

    SCB_CleanDCache();
    __HAL_RCC_DMA2D_CLK_ENABLE();
    DMA2D->CR      = DMA2D_CR_MODE_0;                 /* 記憶體到記憶體＋像素格式轉換 */
    DMA2D->FGMAR   = (uint32_t)src;
    DMA2D->FGOR    = 0u;
    DMA2D->FGPFCCR = DMA2D_INPUT_RGB565;
    DMA2D->OMAR    = (uint32_t)dst;
    DMA2D->OOR     = (uint32_t)(PHYS_W - w);
    DMA2D->OPFCCR  = DMA2D_OUTPUT_RGB565 | DMA2D_OPFCCR_SB;
    DMA2D->NLR     = ((uint32_t)w << DMA2D_NLR_PL_Pos) | (uint32_t)rows;
    DMA2D->CR     |= DMA2D_CR_START;
    for (uint32_t guard = 0; guard < 20000000u; guard++) {
        if ((DMA2D->CR & DMA2D_CR_START) == 0u) { break; }
    }
}

static int  g_full_frames = 2;
static int  g_prev_y0, g_prev_y1;
static bool g_last_hi = true;

static void blit_canvas(void)
{
    const uint16_t *hi = gt7dash_canvas_hi();
    const uint16_t *src = gt7dash_canvas();
    uint16_t       *fb = back_buffer();
    bool            use_hi = hi && BOX[13] != NOHI_MAGIC;

    if (use_hi != g_last_hi) { g_full_frames = 2; g_last_hi = use_hi; }
    if (use_hi) {
        int c0 = 0, c1 = 0;
        if (!gt7dash_canvas_hi_dirty(&c0, &c1)) { c0 = c1 = 0; }
        int y0 = c0, y1 = c1;
        if (g_prev_y1 > g_prev_y0) {
            if (y1 <= y0) { y0 = g_prev_y0; y1 = g_prev_y1; }
            else {
                if (g_prev_y0 < y0) { y0 = g_prev_y0; }
                if (g_prev_y1 > y1) { y1 = g_prev_y1; }
            }
        }
        g_prev_y0 = c0;
        g_prev_y1 = c1;
        if (g_full_frames > 0) { y0 = 0; y1 = GT7DASH_H * SCALE; g_full_frames--; }
        BOX[6] = (uint32_t)(y1 > y0 ? y1 - y0 : 0);
        if (y1 > y0) {
            dma2d_copy_swap(hi + y0 * GT7DASH_W * SCALE, fb + y0 * PHYS_W + OFF_X, y1 - y0);
        }
        return;
    }
    if (!src) { return; }
    for (int y = 0; y < GT7DASH_H; y++) {
        uint32_t *d0 = (uint32_t *)(fb + (y * SCALE) * PHYS_W + OFF_X);
        uint32_t *d1 = (uint32_t *)(fb + (y * SCALE + 1) * PHYS_W + OFF_X);
        const uint16_t *s = src + y * GT7DASH_W;

        for (int x = 0; x < GT7DASH_W; x++) {
            uint16_t v = (uint16_t)((s[x] >> 8) | (s[x] << 8));
            uint32_t two = ((uint32_t)v << 16) | v;
            d0[x] = two;
            d1[x] = two;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 給原作的平台功能：觸控、背光、設定                                     */
/* ------------------------------------------------------------------ */
static bool g_have_touch;

bool racedash_touch(int *x, int *y)
{
    TS_State_t st;
    int        tx, ty;

    if (!g_have_touch) { return false; }
    if (BSP_TS_GetState(0, &st) != BSP_ERROR_NONE || !st.TouchDetected) { return false; }
    /* 面板座標 -> 畫布座標：扣掉左邊黑邊、除以放大倍率。點到黑邊就貼齊畫布邊緣。 */
    tx = ((int)st.TouchX - OFF_X) / SCALE;
    ty = (int)st.TouchY / SCALE;
    if (tx < 0) { tx = 0; }
    if (tx >= GT7DASH_W) { tx = GT7DASH_W - 1; }
    if (ty >= GT7DASH_H) { ty = GT7DASH_H - 1; }
    *x = tx;
    *y = ty;
    return true;
}

/* 原作給 0~255。0 要把背光腳切成 GPIO 拉低：BSP 的 0% 會整數下溢反而全亮
 * （board-notes 14.2，相簿的 brightness_set 同一套）。 */
void racedash_backlight(unsigned char b)
{
    GPIO_InitTypeDef bl = {0};
    uint32_t         pct = ((uint32_t)b * 100u + 254u) / 255u;

    bl.Pull  = GPIO_NOPULL;
    bl.Speed = GPIO_SPEED_FREQ_MEDIUM;
    bl.Pin   = LCD_BL_CTRL_PIN;
    if (pct == 0u) {
        bl.Mode = GPIO_MODE_OUTPUT_PP;
        HAL_GPIO_Init(LCD_BL_CTRL_GPIO_PORT, &bl);
        HAL_GPIO_WritePin(LCD_BL_CTRL_GPIO_PORT, LCD_BL_CTRL_PIN, GPIO_PIN_RESET);
        return;
    }
    bl.Mode      = GPIO_MODE_AF_PP;
    bl.Alternate = LCD_LPTIMx_CHANNEL_AF;
    HAL_GPIO_Init(LCD_BL_CTRL_GPIO_PORT, &bl);
    (void)BSP_LCD_SetBrightness(0, pct);
}

/* 原作的設定（主題、亮度、觸控方向、連線方式）原本存在 ESP32 的 NVS。
 * 這裡放 DTCM：重置後還在，斷電才會回預設。 */
#define PREF_MAGIC  0x50524546u   /* "PREF" */
#define PREF_MAX    16
typedef struct {
    uint32_t magic;
    struct { char key[12]; uint8_t used, value; } e[PREF_MAX];
} prefs_t;
#define PREFS ((volatile prefs_t *)0x20005000u)

static void prefs_check(void)
{
    if (PREFS->magic != PREF_MAGIC) {
        memset((void *)PREFS, 0, sizeof(prefs_t));
        PREFS->magic = PREF_MAGIC;
    }
}

int racedash_pref_get(const char *key, unsigned char *value)
{
    prefs_check();
    for (int i = 0; i < PREF_MAX; i++) {
        if (PREFS->e[i].used && strncmp((const char *)PREFS->e[i].key, key, 11) == 0) {
            *value = PREFS->e[i].value;
            return 1;
        }
    }
    return 0;
}

void racedash_pref_put(const char *key, unsigned char value)
{
    int free_i = -1;

    prefs_check();
    for (int i = 0; i < PREF_MAX; i++) {
        if (PREFS->e[i].used && strncmp((const char *)PREFS->e[i].key, key, 11) == 0) {
            PREFS->e[i].value = value;
            return;
        }
        if (!PREFS->e[i].used && free_i < 0) { free_i = i; }
    }
    if (free_i >= 0) {
        strncpy((char *)PREFS->e[free_i].key, key, 11);
        PREFS->e[free_i].key[11] = 0;
        PREFS->e[free_i].value = value;
        PREFS->e[free_i].used = 1;
    }
}

void racedash_pref_clear(void)
{
    memset((void *)PREFS, 0, sizeof(prefs_t));
    PREFS->magic = PREF_MAGIC;
}

/* ------------------------------------------------------------------ */
/* 展示模式：板子自己產生一行 DSH1（跟 tools/fake_simhub.ps1 一樣的動作）  */
/* ------------------------------------------------------------------ */
static void demo_line(char *out, int cap, uint32_t now, uint32_t period)
{
    static uint32_t seq;
    static int32_t  rpm = 3000;
    static int      gear = 1;
    uint32_t        t = now % 12000u;
    int             thr, brk, speed;
    uint32_t        lap = now % 84000u;

    if (t < 9000u) {
        thr = 100; brk = 0;
        rpm += (int32_t)(9u * period);      /* 每 0.1 秒 +900，間隔變短就等比例變小 */
        if (rpm > 8200 && gear < 6) { gear++; rpm = 5600; }
        if (rpm > 8400) { rpm = 8400; }
    } else {
        thr = 0; brk = 85;
        rpm -= (int32_t)(12u * period);
        if (rpm < 4200 && gear > 2) { gear--; rpm = 6800; }
        if (rpm < 2000) { rpm = 2000; }
    }
    speed = gear * 38 + (rpm - 2000) * 40 / 6500;
    snprintf(out, (size_t)cap,
             "DSH1;%lu;1;%d;%d;%ld;%ld;92;%02lu:%02lu.%03lu;01:24.631;01:23.912;12.8;5;3;10;96;%d;%d;0;0;0;78.4;85.2;96.1;109.3",
             (unsigned long)++seq, speed, gear, (long)rpm, (long)(rpm * 100 / 8500),
             (unsigned long)(lap / 60000u), (unsigned long)((lap / 1000u) % 60u),
             (unsigned long)(lap % 1000u), thr, brk);
}

/* ------------------------------------------------------------------ */
/* 主迴圈                                                               */
/* ------------------------------------------------------------------ */
void gt7dash_set_theme(int theme);   /* port.cpp，除錯用 */

void racedash_run(void)
{
    static char line[512];
    uint32_t    demo_next = 0;

    for (int i = 0; i < 20; i++) {
        if (i != 11) { BOX[i] = 0u; }     /* 展示模式的請求跨重置保留；[20] 也是 */
    }
    BOX[0] = 0x52445348u;                 /* "RDSH" */

    cycle_counter_init();
    framebuffer_mpu_init();
    screen_init();

    {
        TS_Init_t ts;

        ts.Width       = PHYS_W;
        ts.Height      = PHYS_H;
        ts.Orientation = TS_SWAP_NONE;
        ts.Accuracy    = 2;
        g_have_touch = (BSP_TS_Init(0, &ts) == BSP_ERROR_NONE);
    }
    if (!serial_init(SERIAL_BAUD)) {
        Error_Handler();
    }

    /* BOX[20] 寫 "LOWR" 再重置：不配 2 倍鏡像，只跑原作（量原作本身的時間用） */
    gt7dash_hires_set(BOX[20] != LOWR_MAGIC);
    gt7dash_init();

    for (;;) {
        uint32_t now = HAL_GetTick();
        int      n;

        while ((n = serial_read_line(line, sizeof(line))) >= 0) {
            if (n == 0 || BOX[11] == DEMO_MAGIC) { continue; }
            if (gt7dash_feed(line)) { BOX[3]++; } else { BOX[4]++; }
        }
        if (BOX[11] == DEMO_MAGIC && (int32_t)(now - demo_next) >= 0) {
            char demo[160];

            /* 預設跟 SimHub 免費版一樣一秒 10 次；BOX[17] 可改間隔（毫秒），
             * 寫 16 就是授權版的一秒 60 次 */
            uint32_t period = (BOX[17] >= 1u && BOX[17] <= 1000u) ? BOX[17] : 100u;
            demo_next = now + period;
            demo_line(demo, sizeof(demo), now, period);
            if (gt7dash_feed(demo)) { BOX[3]++; } else { BOX[4]++; }
        }
        if ((BOX[12] & 0xFFFF0000u) == 0x54480000u) {
            gt7dash_set_theme((int)(BOX[12] & 0xFFu));
            BOX[12] = 0u;
        }

        {
            uint32_t t0 = DWT->CYCCNT;

            gt7dash_step();
            if (BOX[15] != FROZ_MAGIC) {
                uint32_t t1 = DWT->CYCCNT;
                blit_canvas();
                BOX[14] = DWT->CYCCNT - t1;
                BOX[7] = DWT->CYCCNT - t0;
                if (BOX[7] > BOX[16]) { BOX[16] = BOX[7]; }
                present();
                /* 跟上一格換頁的間隔超過 1.25 個 60Hz 週期（20.8ms）就是掉格 */
                {
                    static uint32_t last_present;
                    uint32_t        t2 = DWT->CYCCNT;
                    if (last_present && t2 - last_present > (SystemCoreClock / 48u)) { BOX[18]++; }
                    BOX[19]++;
                    last_present = t2;
                }
                /* 原作只重畫有變的部分，所以畫布永遠是完整的一格；
                 * 換頁之後兩塊 framebuffer 都要是最新的，下一圈會再貼一次。 */
            }
        }

        BOX[1] = now;
        BOX[2] = g_serial_rx_bytes;
        BOX[5]++;
        BOX[8] = g_serial_rx_overrun;
        BOX[9] = g_serial_line_dropped;
    }
}
