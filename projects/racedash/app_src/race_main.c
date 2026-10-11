/**
 * 賽車儀表：電腦上的 RaceDash Bridge（bridge/）經 ST-LINK 的虛擬 COM 埠（UART4）送遙測，
 * 畫在 5 吋螢幕上。
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
 * [17] 展示模式的資料間隔（毫秒，0 = 16 即一秒 60 次；100 = 一秒 10 次）
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
/* 展示模式：板子自己產生一行 DSH1（跟 RaceDash Bridge 送來的一樣）       */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* 展示模式：板子自己模擬跑圈                                           */
/* ------------------------------------------------------------------ */
/* 不用開遊戲就能看儀表動起來。一圈由直線、煞車、彎道組成：
 * 直線全油門、轉速到頂升檔；進彎前重煞（ABS 閃）、降檔；彎中半油門；
 * 出彎低檔 TC 介入。每圈結束更新上一圈／最佳圈、圈數，油量逐圈下降，
 * 胎溫隨彎道升降，偶爾換名次、偶爾一圈被判無效。
 * 每段的長度與彎速每圈有一點變化，所以每圈時間都不一樣。 */
typedef struct {
    uint16_t ms;        /* 這段的長度 */
    uint8_t  mode;      /* 0 全油門  1 煞車到 target  2 彎中維持 target */
    uint8_t  target;    /* 彎速 km/h */
} demo_seg_t;

static const demo_seg_t k_demo_track[] = {
    {6500, 0,   0}, {1800, 1,  92}, {2600, 2,  92},
    {8200, 0,   0}, {1400, 1, 145}, {1800, 2, 145},
    {5200, 0,   0}, {2200, 1,  68}, {3000, 2,  68},
    {9800, 0,   0}, {1600, 1, 118}, {2400, 2, 118},
    {7000, 0,   0}, {1200, 1, 172}, {1500, 2, 172},
    {4800, 0,   0}, {2000, 1,  84}, {2600, 2,  84},
    {6000, 0,   0},
};
#define DEMO_SEGS    (int)(sizeof(k_demo_track) / sizeof(k_demo_track[0]))
#define DEMO_LAPS    10
#define DEMO_MAX_RPM 8500.0f

static struct {
    bool     started;
    uint32_t last_now, lap_start, rng;
    int      seg, gear, lap, pos;
    uint32_t seg_t, seg_len, last_ms, best_ms;
    float    target, speed, fuel, tyre[4];
    bool     invalid, invalid_next;
} g_sim;

static uint32_t sim_rand(void)
{
    g_sim.rng = g_sim.rng * 1664525u + 1013904223u;
    return g_sim.rng >> 8;
}

/* 進到第 seg 段：長度 ±5%、彎速 ±6% 的變化 */
static void sim_enter_segment(int seg)
{
    const demo_seg_t *d = &k_demo_track[seg];

    g_sim.seg = seg;
    g_sim.seg_t = 0;
    g_sim.seg_len = d->ms * (950u + sim_rand() % 101u) / 1000u;
    g_sim.target = d->target * (0.94f + (float)(sim_rand() % 121u) / 1000.0f);
    if (d->mode == 2 && g_sim.invalid_next && (sim_rand() % 3u) == 0u) {
        g_sim.invalid = true;           /* 這個彎出界，這圈不算 */
        g_sim.invalid_next = false;
    }
}

static void sim_reset(uint32_t now)
{
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.started = true;
    g_sim.rng = now ^ 0x5EED1234u;
    g_sim.last_now = now;
    g_sim.lap_start = now;
    g_sim.gear = 1;
    g_sim.lap = 1;
    g_sim.pos = 5;
    g_sim.fuel = 96.0f;
    for (int i = 0; i < 4; i++) { g_sim.tyre[i] = 74.0f + (float)i; }
    sim_enter_segment(0);
}

static void sim_fmt_lap(char *out, size_t cap, uint32_t ms)
{
    if (ms == 0u) { snprintf(out, cap, "--"); return; }
    snprintf(out, cap, "%02lu:%02lu.%03lu", (unsigned long)(ms / 60000u),
             (unsigned long)((ms / 1000u) % 60u), (unsigned long)(ms % 1000u));
}

static void demo_line(char *out, int cap, uint32_t now)
{
    static const float k_kph_per_krpm[6] = {7.6f, 11.8f, 16.0f, 20.4f, 24.8f, 29.0f};
    static const float k_accel[6] = {30.0f, 24.0f, 18.0f, 13.0f, 9.0f, 6.5f};   /* km/h 每秒 */
    static uint32_t seq;
    const demo_seg_t *d;
    uint32_t dt;
    float    dts, thr = 0.0f, brk = 0.0f, rpm;
    bool     abs_on = false, tc_on = false;
    char     cur[16], last[16], best[16];

    if (!g_sim.started) { sim_reset(now); }
    dt = now - g_sim.last_now;
    if (dt > 100u) { dt = 100u; }
    g_sim.last_now = now;
    dts = (float)dt / 1000.0f;

    /* 這一段跑完就換下一段；最後一段跑完就是一圈 */
    g_sim.seg_t += dt;
    if (g_sim.seg_t >= g_sim.seg_len) {
        int next = g_sim.seg + 1;
        if (next >= DEMO_SEGS) {
            uint32_t lap_ms = now - g_sim.lap_start;
            g_sim.last_ms = lap_ms;
            if (!g_sim.invalid && (g_sim.best_ms == 0u || lap_ms < g_sim.best_ms)) { g_sim.best_ms = lap_ms; }
            g_sim.lap_start = now;
            g_sim.lap = g_sim.lap >= DEMO_LAPS ? 1 : g_sim.lap + 1;
            if (sim_rand() % 3u == 0u) {                        /* 超車或被超 */
                g_sim.pos += (sim_rand() & 1u) ? -1 : 1;
                if (g_sim.pos < 1) { g_sim.pos = 1; }
                if (g_sim.pos > 12) { g_sim.pos = 12; }
            }
            g_sim.invalid = false;
            g_sim.invalid_next = (sim_rand() % 6u) == 0u;
            if (g_sim.fuel < 8.0f) { g_sim.fuel = 100.0f; }   /* 進站加油 */
            next = 0;
        }
        sim_enter_segment(next);
    }
    d = &k_demo_track[g_sim.seg];

    /* 油門、煞車與速度 */
    switch (d->mode) {
    case 0:
        thr = 100.0f;
        g_sim.speed += (k_accel[g_sim.gear - 1] - 0.00012f * g_sim.speed * g_sim.speed) * dts;
        tc_on = g_sim.gear <= 3 && g_sim.seg_t < 1500u && ((now / 70u) & 1u);   /* 出彎打滑 */
        break;
    case 1:
        if (g_sim.speed > g_sim.target + 1.0f) {
            brk = 88.0f;
            g_sim.speed -= (55.0f + 0.00012f * g_sim.speed * g_sim.speed) * dts;
            abs_on = g_sim.speed > 60.0f && g_sim.seg_t < g_sim.seg_len * 6u / 10u && (now / 90u) % 3u != 0u;
        } else {
            thr = 20.0f;
        }
        break;
    default:
        thr = 42.0f + (float)(sim_rand() % 16u);
        g_sim.speed += (g_sim.target - g_sim.speed) * 0.8f * dts;
        break;
    }
    if (g_sim.speed < 0.0f) { g_sim.speed = 0.0f; }

    /* 檔位與轉速：到頂升檔、轉速掉了降檔（煞車時早一點降） */
    rpm = g_sim.speed / k_kph_per_krpm[g_sim.gear - 1] * 1000.0f;
    if (rpm > 8100.0f && g_sim.gear < 6) { g_sim.gear++; }
    else if (rpm < (brk > 0.0f ? 4800.0f : 3300.0f) && g_sim.gear > 1) { g_sim.gear--; }
    rpm = g_sim.speed / k_kph_per_krpm[g_sim.gear - 1] * 1000.0f;
    if (rpm < 1500.0f) { rpm = 1500.0f; }
    if (rpm > DEMO_MAX_RPM) { rpm = DEMO_MAX_RPM; }

    /* 胎溫：彎道和煞車加熱（左右彎輪流），直線慢慢降 */
    {
        bool  left = (g_sim.seg / 3) & 1;
        float heat_f = d->mode == 1 ? 8.0f : d->mode == 2 ? 6.0f : -2.5f;
        float heat_r = d->mode == 2 ? 4.5f : d->mode == 1 ? 2.0f : -2.0f;

        g_sim.tyre[0] += heat_f * (left ? 0.6f : 1.0f) * dts;
        g_sim.tyre[1] += heat_f * (left ? 1.0f : 0.6f) * dts;
        g_sim.tyre[2] += heat_r * (left ? 0.6f : 1.0f) * dts;
        g_sim.tyre[3] += heat_r * (left ? 1.0f : 0.6f) * dts;
        for (int i = 0; i < 4; i++) {
            if (g_sim.tyre[i] < 70.0f) { g_sim.tyre[i] = 70.0f; }
            if (g_sim.tyre[i] > 112.0f) { g_sim.tyre[i] = 112.0f; }
        }
    }

    g_sim.fuel -= thr / 100.0f * 0.09f * dts;
    if (g_sim.fuel < 0.0f) { g_sim.fuel = 0.0f; }

    sim_fmt_lap(cur, sizeof(cur), now - g_sim.lap_start);
    sim_fmt_lap(last, sizeof(last), g_sim.last_ms);
    sim_fmt_lap(best, sizeof(best), g_sim.best_ms);
    /* 欄位順序見 bridge/README.md 的 DSH1 格式 */
    snprintf(out, (size_t)cap,
             "DSH1;%lu;1;%d;%d;%d;%d;92;%s;%s;%s;%.1f;%d;%d;%d;%d;%d;%d;%d;%d;%d;%.1f;%.1f;%.1f;%.1f",
             (unsigned long)++seq, (int)(g_sim.speed + 0.5f), g_sim.gear, (int)rpm,
             (int)(rpm * 100.0f / DEMO_MAX_RPM), cur, last, best,
             (double)(g_sim.fuel / 4.5f), g_sim.pos, g_sim.lap, DEMO_LAPS, (int)(g_sim.fuel + 0.5f),
             (int)thr, (int)brk, tc_on ? 1 : 0, abs_on ? 1 : 0, g_sim.invalid ? 1 : 0,
             (double)g_sim.tyre[0], (double)g_sim.tyre[1], (double)g_sim.tyre[2], (double)g_sim.tyre[3]);
}

/* ------------------------------------------------------------------ */
/* 左右黑邊的按鈕                                                       */
/* ------------------------------------------------------------------ */
/* 原作的畫面只佔中間 640 像素，左右各 80 像素黑邊沒有用到，放三顆按鈕：
 *   左上「<」上一個主題   右邊「>」下一個主題（跟原作選單裡選一樣，會記住）
 *   左下「DEMO」：點一下開始模擬跑圈，再點一下回到遊戲的資料（亮橘色 = 模擬中）
 * 原作的設定選單開著時，切主題的按鈕不動作。黑邊的觸控都不交給原作
 * （以前點到黑邊會當成點畫面邊緣）。按住時按鈕變亮。 */
enum { BTN_PREV, BTN_NEXT, BTN_DEMO, BTN_COUNT, BTN_NONE = -1 };

static const struct { int16_t x0, y0, x1, y1; } k_btn[BTN_COUNT] = {
    {6,                  12,  74,                 392},   /* 上一個主題 */
    {PHYS_W - 74,        12,  PHYS_W - 6,         468},   /* 下一個主題 */
    {6,                  404, 74,                 468},   /* DEMO */
};

static const uint8_t k_demo_glyph[4][7] = {   /* 5x7：D E M O，每列低 5 位元 */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
};

/* 點到線段的距離平方 */
static float seg_dist2(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    if (t < 0.0f) { t = 0.0f; }
    if (t > 1.0f) { t = 1.0f; }
    dx = px - (ax + t * dx);
    dy = py - (ay + t * dy);
    return dx * dx + dy * dy;
}

static void draw_button(int b, bool pressed, bool active)
{
    const uint16_t orange = 0xFC60u, grey = 0x52AAu, light = 0x9CD3u, dark = 0x2124u;
    const int      x0 = k_btn[b].x0, y0 = k_btn[b].y0, x1 = k_btn[b].x1, y1 = k_btn[b].y1;
    const uint16_t fill = active ? orange : pressed ? dark : 0x0000u;
    const uint16_t edge = active ? orange : pressed ? light : grey;
    const uint16_t ink  = active ? 0x0000u : pressed ? 0xFFFFu : light;
    const float    cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;

    for (int f = 0; f < 2; f++) {                       /* 兩塊 framebuffer 都畫 */
        uint16_t *fb = (uint16_t *)(f ? FB1_ADDR : FB0_ADDR);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                int  ax = x - x0, bx = x1 - 1 - x, ay = y - y0, by = y1 - 1 - y;
                bool cut = ax + ay < 4 || bx + ay < 4 || ax + by < 4 || bx + by < 4;   /* 切角 */
                bool border = ax < 2 || bx < 2 || ay < 2 || by < 2 ||
                              ax + ay < 6 || bx + ay < 6 || ax + by < 6 || bx + by < 6;
                uint16_t c = cut ? 0x0000u : border ? edge : fill;
                if (!cut && !border && b != BTN_DEMO) {
                    /* 箭頭：兩條粗線組成的 < 或 > */
                    float s = b == BTN_PREV ? -1.0f : 1.0f;
                    float px = x + 0.5f, py = y + 0.5f;
                    float tipx = cx + s * 11.0f, armx = cx - s * 9.0f;
                    if (seg_dist2(px, py, tipx, cy, armx, cy - 20.0f) < 12.0f ||
                        seg_dist2(px, py, tipx, cy, armx, cy + 20.0f) < 12.0f) { c = ink; }
                }
                fb[y * PHYS_W + x] = c;
            }
        }
        if (b == BTN_DEMO) {
            const int sc = 2, pitch = 6 * sc;
            const int tx = (x0 + x1 - (4 * pitch - sc)) / 2, ty = (y0 + y1 - 7 * sc) / 2;
            for (int ch = 0; ch < 4; ch++) {
                for (int r = 0; r < 7; r++) {
                    for (int bit = 0; bit < 5; bit++) {
                        if (!(k_demo_glyph[ch][r] & (0x10u >> bit))) { continue; }
                        for (int yy = 0; yy < sc; yy++) {
                            for (int xx = 0; xx < sc; xx++) {
                                fb[(ty + r * sc + yy) * PHYS_W + tx + ch * pitch + bit * sc + xx] = ink;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void draw_side_buttons(void)
{
    for (int b = 0; b < BTN_COUNT; b++) {
        draw_button(b, false, b == BTN_DEMO && BOX[11] == DEMO_MAGIC);
    }
}

/* 黑邊上的哪一顆按鈕（沒有就 BTN_NONE；黑邊上沒按鈕的地方也算黑邊） */
static int side_button_at(int x, int y)
{
    for (int b = 0; b < BTN_COUNT; b++) {
        if (x >= k_btn[b].x0 - 6 && x < k_btn[b].x1 + 6 && y >= k_btn[b].y0 - 6 && y < k_btn[b].y1 + 6) {
            return b;
        }
    }
    return BTN_NONE;
}

/* 每格讀一次觸控：黑邊上的按鈕自己處理（按下時動作、放開時恢復外觀），
 * 畫面中間的觸控留給原作讀（racedash_touch）。 */
static bool    g_ts_down;
static int16_t g_ts_x, g_ts_y;

static void poll_touch(void)
{
    static int held = BTN_NONE;     /* 按著的按鈕；在黑邊上但不是按鈕也算按著（BTN_COUNT） */
    TS_State_t st;
    int        x, y;

    g_ts_down = false;
    if (!g_have_touch || BSP_TS_GetState(0, &st) != BSP_ERROR_NONE || !st.TouchDetected) {
        if (held >= 0 && held < BTN_COUNT) {
            draw_button(held, false, held == BTN_DEMO && BOX[11] == DEMO_MAGIC);
        }
        held = BTN_NONE;
        return;
    }
    x = (int)st.TouchX;
    y = (int)st.TouchY;
    if (x >= OFF_X && x < OFF_X + GT7DASH_W * SCALE && held == BTN_NONE) {
        g_ts_down = true;           /* 畫面中間：交給原作 */
        g_ts_x = (int16_t)x;
        g_ts_y = (int16_t)y;
        return;
    }
    if (held != BTN_NONE) { return; }   /* 同一次按壓只動作一次 */

    held = side_button_at(x, y);
    if (held == BTN_NONE) { held = BTN_COUNT; return; }
    switch (held) {
    case BTN_PREV:
    case BTN_NEXT:
        if (!gt7dash_step_theme(held == BTN_PREV ? -1 : 1)) { held = BTN_COUNT; return; }
        draw_button(held, true, false);
        break;
    default: {
        bool on = BOX[11] != DEMO_MAGIC;
        BOX[11] = on ? DEMO_MAGIC : 0u;
        if (on) { g_sim.started = false; }      /* 每次開始都從出站重新跑 */
        draw_button(BTN_DEMO, true, on);
        break;
    }
    }
}

bool racedash_touch(int *x, int *y)
{
    int tx, ty;

    if (!g_ts_down) { return false; }
    /* 面板座標 -> 畫布座標：扣掉左邊黑邊、除以放大倍率。點到右邊黑邊就貼齊畫布邊緣。 */
    tx = ((int)g_ts_x - OFF_X) / SCALE;
    ty = (int)g_ts_y / SCALE;
    if (tx < 0) { tx = 0; }
    if (tx >= GT7DASH_W) { tx = GT7DASH_W - 1; }
    if (ty >= GT7DASH_H) { ty = GT7DASH_H - 1; }
    *x = tx;
    *y = ty;
    return true;
}

/* ------------------------------------------------------------------ */
/* 主迴圈                                                               */
/* ------------------------------------------------------------------ */

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
    draw_side_buttons();

    for (;;) {
        uint32_t now = HAL_GetTick();
        int      n;

        poll_touch();

        while ((n = serial_read_line(line, sizeof(line))) >= 0) {
            if (n == 0 || BOX[11] == DEMO_MAGIC) { continue; }
            if (gt7dash_feed(line)) { BOX[3]++; } else { BOX[4]++; }
        }
        if (BOX[11] == DEMO_MAGIC && (int32_t)(now - demo_next) >= 0) {
            char demo[160];

            /* 預設一秒 60 次（跟 RaceDash Bridge 一樣，轉速條最順）；BOX[17] 可改間隔
             * （毫秒），寫 100 就是免費版的一秒 10 次 */
            uint32_t period = (BOX[17] >= 1u && BOX[17] <= 1000u) ? BOX[17] : 16u;
            demo_next = now + period;
            demo_line(demo, sizeof(demo), now);
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
