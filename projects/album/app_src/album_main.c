/**
 * STM32H7S78-DK 電子相簿。
 *
 * 面板是 800x480 橫式，但照片是直式，所以整個 UI 跟照片都轉 90 度當成
 * 480x800 直立畫面用（gfx.c 負責座標轉換）。
 *
 * 流程：掛載 SD -> 掃描一次建立完整照片清單 -> 勾選最上層資料夾 -> 隨機播放。
 * 勾選只是過濾既有清單，不會重新掃卡。
 *
 * 整支程式對 SD 卡只讀不寫，磁碟層也回報寫入保護。
 */
#include "main.h"
#include "stm32h7s78_discovery.h"
#include "stm32h7s78_discovery_lcd.h"
#include "stm32h7s78_discovery_ts.h"
#include "stm32h7s78_discovery_sd.h"

#include "ff.h"
#include "ff_gen_drv.h"

#include "gfx.h"
#include "photo.h"

#include <string.h>
#include <stdbool.h>

extern const Diskio_drvTypeDef SD_BSP_Driver;

/* 1 = 開機解一張就停住，方便用 SWD 把緩衝區讀出來對照。 */
#define DEBUG_ONE_SHOT  0

#define FB0_ADDR        LCD_LAYER_0_ADDRESS      /* 0x90000000 */
#define FB1_ADDR        LCD_LAYER_1_ADDRESS      /* 0x90200000 */

#define COL_BG          RGB565(16, 18, 24)
#define COL_TEXT        RGB565(236, 238, 245)
#define COL_DIM         RGB565(140, 146, 160)
#define COL_ACCENT      RGB565(78, 154, 241)
#define COL_PANEL       RGB565(30, 34, 44)
#define COL_LINE        RGB565(54, 60, 74)

#define MAX_TOP         64U
#define MAX_PHOTOS      8192U
/* 完整路徑上限。中文/日文一個字 3 bytes，加上多層子資料夾很容易超過 128，
 * 超過的照片會被跳過。PSRAM 還有空間，放寬到 256 免得使用者莫名其妙少照片。 */
#define PATH_MAX        256U
#define NAME_MAX        96U
#define MAX_DEPTH       10U
#define SCAN_PATH_LEN   512U

/* 清單放 PSRAM：8192 x 256 = 2MB，內部 RAM 放不下。
 * 位置接在 photo.c 的縮圖緩衝區之後（見那邊的記憶體配置表）。 */
#define PLAYLIST_BASE   ((char *)0x91C00000u)     /* 8192 x 256 = 2 MB */
#define TOPIDX_BASE     ((uint8_t *)0x91E00000u)  /* 8 KB */

typedef struct {
    char     name[NAME_MAX];
    uint32_t photos;
    bool     selected;
} top_t;

static top_t    g_top[MAX_TOP];
static uint32_t g_top_count;
static uint32_t g_photo_count;

static uint16_t g_order[MAX_PHOTOS];
static uint32_t g_order_count;

static FATFS    g_fs;
static char     g_drive[4];
static DIR      g_dir[MAX_DEPTH];
static FILINFO  g_fno[MAX_DEPTH];
static char     g_scan_path[SCAN_PATH_LEN];
static uint32_t g_cur_top;

static uint32_t g_front;
static uint32_t g_interval_s = 3;      /* 預設 3 秒，可調 2~5 */

/* 診斷用，SWD 讀得到。 */
volatile uint32_t g_stage;
volatile int32_t  g_err;
volatile uint32_t g_decode_ok;
volatile uint32_t g_decode_fail;
volatile uint32_t g_last_ms;
volatile uint32_t g_exit_touch;   /* 因觸控離開播放的次數 */
volatile uint32_t g_skipped_long; /* 路徑太長被跳過的照片數 */
volatile uint32_t g_skipped_full; /* 清單滿了之後被跳過的照片數 */
volatile uint32_t g_paused;       /* 1 = 播放暫停中（畫面凍結）*/

/* ------------------------------------------------------------------ */
/* 基礎設施                                                            */
/* ------------------------------------------------------------------ */

/* PSRAM 設成 write-through：CPU 的寫入直接落到記憶體，LTDC 從 PSRAM 讀才不會
 * 讀到還留在快取裡的舊內容。整片 32MB 都設，因為解碼緩衝區也在這裡面。 */
static void psram_mpu_init(void)
{
    MPU_Region_InitTypeDef mpu = {0};

    HAL_MPU_Disable();
    mpu.Enable           = MPU_REGION_ENABLE;
    mpu.Number           = MPU_REGION_NUMBER7;
    mpu.BaseAddress      = 0x90000000u;
    mpu.Size             = MPU_REGION_SIZE_32MB;
    mpu.AccessPermission = MPU_REGION_FULL_ACCESS;
    mpu.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    mpu.IsCacheable      = MPU_ACCESS_CACHEABLE;
    mpu.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;   /* write-through */
    mpu.TypeExtField     = MPU_TEX_LEVEL0;
    mpu.SubRegionDisable = 0x00;
    mpu.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    HAL_MPU_ConfigRegion(&mpu);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

    SCB_CleanInvalidateDCache();
}

static void screen_init(void)
{
    /* BSP 預設 ARGB8888（每像素 4 bytes），我們畫的是 16 位元，
     * 不指定 RGB565 的話會用錯的列距去讀，畫面會橫向重複。 */
    if (BSP_LCD_InitEx(0, LCD_ORIENTATION_LANDSCAPE, LCD_PIXEL_FORMAT_RGB565,
                       PHYS_W, PHYS_H) != BSP_ERROR_NONE) {
        Error_Handler();
    }

    for (uint32_t i = 0; i < (uint32_t)PHYS_W * PHYS_H; i++) {
        ((uint16_t *)FB0_ADDR)[i] = 0;
        ((uint16_t *)FB1_ADDR)[i] = 0;
    }

    BSP_LCD_SetLayerAddress(0, 0, FB0_ADDR);
    /* no-reload：位址只寫進 shadow register，等我們要求垂直消隱重載才生效。
     * 否則 SetLayerAddress 會在畫面掃描途中重寫整組暫存器而撕裂。 */
    BSP_LCD_Reload(0, BSP_LCD_RELOAD_NONE);
    BSP_LCD_DisplayOn(0);

    g_front = 0;
    gfx_set_framebuffer((uint16_t *)FB1_ADDR);
}

static void present(void)
{
    uint32_t drawn = g_front ? FB0_ADDR : FB1_ADDR;

    /* 只寫 CFBAR 一個暫存器：它是 shadow register，寫入不會立即生效，
     * 等垂直消隱重載時整批原子切換，中間沒有半完成狀態。 */
    LTDC_Layer1->CFBAR = drawn;
    LTDC->SRCR = LTDC_SRCR_VBR;
    for (uint32_t guard = 0; guard < 2000000u; guard++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) {
            break;
        }
    }

    g_front ^= 1u;
    gfx_set_framebuffer((uint16_t *)(g_front ? FB0_ADDR : FB1_ADDR));
}

static void rnd_mix(uint32_t v);      /* 定義在下面的亂數區塊 */
static void watchdog_feed(void);      /* 定義在下面的記憶卡狀態區塊 */
static bool sd_present(void);
static void show_scan_progress(void);

/* 觸控座標換成直立座標。
 * gfx 把邏輯 (x,y) 映到實體 offset (GFX_W-1-x)*PHYS_W + y，
 * 也就是實體列 = GFX_W-1-x、實體行 = y，反過來就是下面這樣。 */
static bool read_touch(int *x, int *y)
{
    TS_State_t st;

    if (BSP_TS_GetState(0, &st) != BSP_ERROR_NONE || !st.TouchDetected) {
        return false;
    }
    *x = (int)GFX_W - 1 - (int)st.TouchY;
    *y = (int)st.TouchX;

    /* 每一次觸控都是一次熵注入。 */
    rnd_mix(HAL_GetTick() + ((uint32_t)*x << 16) + (uint32_t)*y);
    return true;
}

/* 等到手指真的放開。
 *
 * 電容觸控在放開的瞬間會彈跳，讀到「沒碰到」不代表真的離開了。要連續數次
 * 都讀不到才算數，否則殘留的事件會被下一段程式碼當成新的觸碰 —— 進入播放
 * 後第一張照片立刻被判定成「使用者要離開」就是這樣來的。 */
static void wait_release(void)
{
    uint32_t t0 = HAL_GetTick();
    int clean = 0;
    int x, y;

    while (clean < 4) {
        watchdog_feed();
        if (read_touch(&x, &y)) {
            clean = 0;
            if (HAL_GetTick() - t0 > 3000u) {
                break;                  /* 觸控卡住就不要一直等 */
            }
        } else {
            clean++;
        }
        HAL_Delay(25);
    }
    HAL_Delay(120);
}

/* 偵測一次「確實的」觸碰：要連續讀到才算，避免雜訊誤觸發。 */
static bool touch_confirmed(void)
{
    int x, y;

    if (!read_touch(&x, &y)) {
        return false;
    }
    /* 原本要連續 4 次讀到才算數，短按很容易在中間被漏掉，變成「點不到」。
     * 兩次已經足以濾掉雜訊，反應也快得多。 */
    HAL_Delay(20);
    return read_touch(&x, &y);
}

static uint32_t rnd_state = 0x2545F491u;

/* 把外部熵混進種子。
 *
 * 原本只用「開機到掃描完的時間」當種子，但那段耗時相當固定（SD 初始化加上
 * 掃描固定數量的檔案），每次開機常常拿到同一個毫秒數 —— 同種子就是同一組
 * 順序，看起來就像沒有隨機。
 *
 * 使用者按下螢幕的時間點才是真正不可預測的：以毫秒計沒有人按得出重複，
 * 再加上 CPU 週期計數器（600MHz，同一毫秒內還有六十萬個刻度）與觸控座標，
 * 每一次觸碰都會讓後續的順序完全不同。 */
static void rnd_mix(uint32_t v)
{
    rnd_state ^= v * 2654435761u;
    rnd_state ^= DWT->CYCCNT;
    rnd_state = (rnd_state << 7) | (rnd_state >> 25);
    if (rnd_state == 0u) {
        rnd_state = 0x2545F491u;
    }
}

static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

/* ------------------------------------------------------------------ */
/* 記憶卡狀態與看門狗                                                  */
/* ------------------------------------------------------------------ */

static IWDG_HandleTypeDef g_iwdg;
static bool g_wdt_on;

/* 獨立看門狗，用來兜住「卡片沒回應」這個沒辦法從程式裡跳出來的情況。
 *
 * BSP_SD_Init() 最終會走到 HAL 的 SD_InitCard()，裡面有
 *   while (sd_rca == 0U)
 * 這個迴圈沒有逾時保護。卡片若停在異常狀態不回應 CMD3，程式就永遠出不來，
 * 畫面停在「載入中」而且沒有任何提示 —— 使用者只會覺得機器壞了。
 *
 * 同一個執行緒沒辦法讓自己跳出無窮迴圈，只能靠硬體。逾時設 16 秒，正常
 * 操作（解一張照片約 0.5 秒、掃描一次約 1 秒）遠遠用不到。 */
static void watchdog_start(void)
{
    g_iwdg.Instance       = IWDG;
    g_iwdg.Init.Prescaler = IWDG_PRESCALER_256;   /* LSI 32kHz / 256 = 125Hz */
    g_iwdg.Init.Reload    = 2000u;                /* 2000 / 125 = 16 秒 */
    g_iwdg.Init.Window    = IWDG_WINDOW_DISABLE;
    if (HAL_IWDG_Init(&g_iwdg) == HAL_OK) {
        g_wdt_on = true;
    }
}

/* 所有可能久留的迴圈裡都要餵，否則會被誤判成當機而重置。 */
static void watchdog_feed(void)
{
    if (g_wdt_on) {
        (void)HAL_IWDG_Refresh(&g_iwdg);
    }
}

static bool sd_present(void)
{
    return BSP_SD_IsDetected(0) == SD_PRESENT;
}

/* ------------------------------------------------------------------ */
/* USER 按鈕：螢幕開關                                                 */
/* ------------------------------------------------------------------ */

static int32_t g_btn_idle;      /* 開機時的電位，當成「沒按」的基準 */
static int32_t g_btn_last;
static bool    g_screen_on = true;

/* 不假設按鈕是高電位還是低電位觸發，開機時量到什麼就當成放開的狀態，
 * 之後偏離這個值就是按下。這樣換板子或改接線都不用回來改程式。 */
static void button_init(void)
{
    (void)BSP_PB_Init(BUTTON_USER, BUTTON_MODE_GPIO);
    g_btn_idle = BSP_PB_GetState(BUTTON_USER);
    g_btn_last = g_btn_idle;
}

/* 偵測一次「按下」的邊緣。要連續兩次讀到同一個值才採信，濾掉接點彈跳。 */
static bool button_pressed(void)
{
    int32_t now = BSP_PB_GetState(BUTTON_USER);
    bool    edge;

    if (now != g_btn_last) {
        HAL_Delay(15);
        if (BSP_PB_GetState(BUTTON_USER) != now) {
            return false;               /* 彈跳，不算 */
        }
    }
    edge = (now != g_btn_idle) && (g_btn_last == g_btn_idle);
    g_btn_last = now;
    return edge;
}

/* 關螢幕時連播放一起暫停。
 *
 * 「螢幕關掉但背景繼續解碼」在這個專案裡沒有任何好處：順序是隨機的，使用者
 * 分不出「現在這張」和「暫停那張」的差別，卻要付出每 2~5 秒一次全速解碼的
 * 電力、以及整晚無意義的記憶卡讀取。背景運算要有價值，前提是背景有別的東西
 * 在跑（時鐘、網路同步之類），目前沒有。 */
static void screen_set(bool on)
{
    if (on == g_screen_on) {
        return;
    }
    g_screen_on = on;

    if (!on) {
        (void)BSP_LCD_DisplayOff(0);
        return;
    }

    (void)BSP_LCD_DisplayOn(0);

    /* BSP 的 DisplayOn 不會把背光打開，這是 ST 兩邊不對稱的地方：
     *
     *   DisplayOff  把背光腳（GPIOG15）從 LPTIM 的 PWM 替代功能改成一般
     *               輸出，而 ODR 是 0，等於把背光關掉
     *   DisplayOn   只做 __HAL_LTDC_ENABLE 和拉高 LCD_DISP_EN，
     *               完全沒碰背光腳
     *
     * 結果就是「關得掉、開不回來」：LTDC 恢復了、面板也 enable 了，就是沒有
     * 背光。這裡直接把那支腳拉高（全亮）。本專案不用亮度調節，所以維持一般
     * 輸出即可，不必還原成 PWM。 */
    {
        GPIO_InitTypeDef bl = {0};

        LCD_BL_CTRL_GPIO_CLK_ENABLE();
        bl.Mode  = GPIO_MODE_OUTPUT_PP;
        bl.Pull  = GPIO_NOPULL;
        bl.Speed = GPIO_SPEED_FREQ_MEDIUM;
        bl.Pin   = LCD_BL_CTRL_PIN;
        HAL_GPIO_Init(LCD_BL_CTRL_GPIO_PORT, &bl);
        HAL_GPIO_WritePin(LCD_BL_CTRL_GPIO_PORT, LCD_BL_CTRL_PIN, GPIO_PIN_SET);
    }
}

/* 放在每個迴圈裡。回傳 true 代表螢幕是亮的（可以接受觸控）。 */
static bool screen_poll(void)
{
    if (button_pressed()) {
        screen_set(!g_screen_on);
    }
    return g_screen_on;
}

/* ------------------------------------------------------------------ */
/* 掃描                                                                */
/* ------------------------------------------------------------------ */

static bool is_jpeg(const char *name)
{
    size_t n = strlen(name);
    const char *p;

    if (n >= 4U) {
        p = name + n - 4;
        if (p[0] == '.' && (p[1] | 32) == 'j' && (p[2] | 32) == 'p' &&
            (p[3] | 32) == 'g') {
            return true;
        }
    }
    if (n >= 5U) {
        p = name + n - 5;
        if (p[0] == '.' && (p[1] | 32) == 'j' && (p[2] | 32) == 'p' &&
            (p[3] | 32) == 'e' && (p[4] | 32) == 'g') {
            return true;
        }
    }
    return false;
}

static void add_photo(const char *path, uint32_t top)
{
    char *slot;

    if (g_photo_count >= MAX_PHOTOS) {
        g_skipped_full++;
        return;
    }
    if (strlen(path) >= PATH_MAX) {
        g_skipped_long++;
        return;
    }
    slot = PLAYLIST_BASE + g_photo_count * PATH_MAX;
    strncpy(slot, path, PATH_MAX - 1U);
    slot[PATH_MAX - 1U] = 0;
    TOPIDX_BASE[g_photo_count] = (uint8_t)top;
    g_photo_count++;
}

/* DIR 與 FILINFO 依深度放 BSS：FILINFO 帶 UTF-8 長檔名緩衝區約 800 位元組，
 * 遞迴十層放堆疊會爆掉。 */
static void scan(uint32_t depth)
{
    size_t base = strlen(g_scan_path);

    if (depth >= MAX_DEPTH) {
        return;
    }
    if (f_opendir(&g_dir[depth], g_scan_path) != FR_OK) {
        return;
    }

    for (;;) {
        FILINFO *fi = &g_fno[depth];
        size_t len;

        if (f_readdir(&g_dir[depth], fi) != FR_OK || fi->fname[0] == 0) {
            break;
        }
        if (fi->fname[0] == '.' || (fi->fattrib & (AM_SYS | AM_HID))) {
            continue;
        }
        watchdog_feed();

        len = strlen(fi->fname);
        if (base + 1U + len + 1U > SCAN_PATH_LEN) {
            continue;
        }
        g_scan_path[base] = '/';
        memcpy(&g_scan_path[base + 1U], fi->fname, len + 1U);

        if (fi->fattrib & AM_DIR) {
            if (depth == 0U) {
                if (g_top_count >= MAX_TOP) {
                    g_scan_path[base] = 0;
                    continue;
                }
                g_cur_top = g_top_count;
                strncpy(g_top[g_cur_top].name, fi->fname, NAME_MAX - 1U);
                g_top[g_cur_top].name[NAME_MAX - 1U] = 0;
                g_top[g_cur_top].selected = true;   /* 預設全選 */
                g_top_count++;
            }
            scan(depth + 1U);
        } else if (is_jpeg(fi->fname) && depth > 0U && g_cur_top < MAX_TOP) {
            add_photo(g_scan_path, g_cur_top);
            g_top[g_cur_top].photos++;

            /* 掃幾萬個檔案要花上數十秒，畫面一直停在「載入中」會像當機。
             * 每 200 張更新一次數字，讓使用者看得到它在動。 */
            if ((g_photo_count % 200u) == 0u) {
                show_scan_progress();
            }
        }

        g_scan_path[base] = 0;
    }

    f_closedir(&g_dir[depth]);
    g_scan_path[base] = 0;
}

static uint32_t selected_photo_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_photo_count; i++) {
        uint32_t t = TOPIDX_BASE[i];
        if (t < g_top_count && g_top[t].selected) {
            n++;
        }
    }
    return n;
}

/* 依目前的勾選建立播放順序，然後洗牌。 */
static void build_order(void)
{
    g_order_count = 0;
    for (uint32_t i = 0; i < g_photo_count; i++) {
        uint32_t t = TOPIDX_BASE[i];
        if (t < g_top_count && g_top[t].selected) {
            g_order[g_order_count++] = (uint16_t)i;
        }
    }

    /* Fisher-Yates。每次進播放都重洗，播完一輪也重洗，
     * 所以同一批照片不會每輪都同一個順序。 */
    for (uint32_t i = g_order_count; i > 1U; i--) {
        uint32_t j = rnd() % i;
        uint16_t tmp = g_order[i - 1U];
        g_order[i - 1U] = g_order[j];
        g_order[j] = tmp;
    }
}

/* ------------------------------------------------------------------ */
/* 選擇畫面                                                            */
/* ------------------------------------------------------------------ */

#define ROW_Y0       120
#define ROW_H        58
#define ROWS_VISIBLE 7
#define BTN_ALL_Y    580
#define BTN_ALL_H    48
#define IV_Y         650
#define IV_H         48
#define IV_X0        110
#define IV_W         74
#define IV_GAP       10
#define BTN_GO_Y     720
#define BTN_GO_H     62

static uint32_t g_scroll;

static void draw_checkbox(int x, int y, bool on)
{
    gfx_rect(x, y, 32, 32, on ? COL_ACCENT : COL_LINE);
    gfx_rect(x + 1, y + 1, 30, 30, on ? COL_ACCENT : COL_LINE);
    if (on) {
        gfx_fill_rect(x + 8, y + 8, 16, 16, COL_ACCENT);
    }
}

/* 名稱太長就從尾端截掉，避免壓到右邊的張數。 */
static void draw_name_clipped(int x, int y, const char *s, int limit,
                              uint16_t color)
{
    char buf[NAME_MAX];

    strncpy(buf, s, NAME_MAX - 1U);
    buf[NAME_MAX - 1U] = 0;

    while (gfx_text_width(buf) > limit) {
        size_t len = strlen(buf);
        if (len == 0U) {
            break;
        }
        /* 往前退到不是 UTF-8 接續位元組的位置，才不會砍出半個字。 */
        do {
            len--;
        } while (len > 0U && ((buf[len] & 0xC0) == 0x80));
        buf[len] = 0;
    }
    gfx_text(x, y, buf, color);
}

static void draw_select_screen(void)
{
    uint32_t shown;

    gfx_clear(COL_BG);
    gfx_text_center(GFX_W / 2, 24, "電子相簿", COL_TEXT);
    gfx_text_center(GFX_W / 2, 62, "選擇要播放的資料夾", COL_DIM);

    shown = g_top_count - g_scroll;
    if (shown > ROWS_VISIBLE) {
        shown = ROWS_VISIBLE;
    }

    for (uint32_t i = 0; i < shown; i++) {
        uint32_t idx = g_scroll + i;
        int y = ROW_Y0 + (int)i * ROW_H;

        gfx_fill_rect(16, y, GFX_W - 32, ROW_H - 8, COL_PANEL);
        draw_checkbox(28, y + 9, g_top[idx].selected);
        draw_name_clipped(76, y + 15, g_top[idx].name,
                          GFX_W - 76 - 96, COL_TEXT);
        gfx_number_right(GFX_W - 28, y + 15, g_top[idx].photos, COL_DIM);
    }

    if (g_top_count > ROWS_VISIBLE) {
        gfx_text_center(GFX_W / 2, ROW_Y0 + ROWS_VISIBLE * ROW_H + 6,
                        "上下滑動看更多", COL_DIM);
    }

    gfx_pill(16, BTN_ALL_Y, GFX_W - 32, BTN_ALL_H, COL_PANEL);
    gfx_text_center(GFX_W / 2, BTN_ALL_Y + 14, "全部選取或取消", COL_TEXT);

    gfx_text(20, IV_Y + 14, "間隔", COL_DIM);
    for (uint32_t s = 2; s <= 5; s++) {
        int  x  = IV_X0 + (int)(s - 2) * (IV_W + IV_GAP);
        bool on = (g_interval_s == s);
        gfx_pill(x, IV_Y, IV_W, IV_H, on ? COL_ACCENT : COL_PANEL);
        gfx_number(x + 20, IV_Y + 14, s, on ? COL_BG : COL_TEXT);
        gfx_text(x + 40, IV_Y + 14, "秒", on ? COL_BG : COL_TEXT);
    }

    {
        uint32_t n = selected_photo_count();
        gfx_pill(16, BTN_GO_Y, GFX_W - 32, BTN_GO_H,
                 n ? COL_ACCENT : COL_PANEL);
        if (n) {
            gfx_text_center(GFX_W / 2, BTN_GO_Y + 12, "開始播放", COL_BG);
            gfx_number_right(GFX_W - 40, BTN_GO_Y + 12, n, COL_BG);
        } else {
            gfx_text_center(GFX_W / 2, BTN_GO_Y + 20,
                            "沒有選取任何資料夾", COL_DIM);
        }
    }
}

/* 兩塊 buffer 都要畫，否則交換之後會看到上一次的內容。 */
static void repaint_select(void)
{
    for (int i = 0; i < 2; i++) {
        draw_select_screen();
        present();
    }
}

/* 回傳 true 表示要開始播放。 */
static bool select_screen(void)
{
    bool dirty = true;

    for (;;) {
        int x, y;

        if (dirty) {
            repaint_select();
            dirty = false;
        }

        watchdog_feed();
        if (!sd_present()) {
            return false;               /* 卡片被拔掉，交回上層處理 */
        }
        if (!screen_poll()) {
            /* 螢幕關著時不吃觸控，否則會在看不見的情況下改到設定。 */
            HAL_Delay(30);
            continue;
        }

        if (!read_touch(&x, &y)) {
            HAL_Delay(15);
            continue;
        }

        if (y >= ROW_Y0 && y < ROW_Y0 + ROWS_VISIBLE * ROW_H) {
            uint32_t i = g_scroll + (uint32_t)(y - ROW_Y0) / ROW_H;
            if (i < g_top_count) {
                g_top[i].selected = !g_top[i].selected;
                dirty = true;
            }
        } else if (y >= BTN_ALL_Y && y < BTN_ALL_Y + BTN_ALL_H) {
            bool any = false;
            for (uint32_t i = 0; i < g_top_count; i++) {
                if (g_top[i].selected) {
                    any = true;
                }
            }
            for (uint32_t i = 0; i < g_top_count; i++) {
                g_top[i].selected = !any;
            }
            dirty = true;
        } else if (y >= IV_Y && y < IV_Y + IV_H) {
            for (uint32_t s = 2; s <= 5; s++) {
                int bx = IV_X0 + (int)(s - 2) * (IV_W + IV_GAP);
                if (x >= bx && x < bx + IV_W) {
                    g_interval_s = s;
                    dirty = true;
                }
            }
        } else if (y >= BTN_GO_Y && y < BTN_GO_Y + BTN_GO_H) {
            if (selected_photo_count() > 0U) {
                build_order();
                wait_release();
                return true;
            }
        }

        wait_release();
    }
}

/* ------------------------------------------------------------------ */
/* 播放                                                                */
/* ------------------------------------------------------------------ */

/* 掃描進度。數字會一直跳，讓使用者知道它在動而不是當掉了。 */
static void show_scan_progress(void)
{
    watchdog_feed();
    for (int i = 0; i < 2; i++) {
        gfx_clear(COL_BG);
        gfx_text_center(GFX_W / 2, GFX_H / 2 - 60, "掃描記憶卡", COL_TEXT);
        gfx_number_right(GFX_W / 2 + 20, GFX_H / 2, g_photo_count, COL_ACCENT);
        gfx_text(GFX_W / 2 + 28, GFX_H / 2, "張", COL_DIM);
        present();
    }
}

static void show_message(const char *line1, const char *line2)
{
    for (int i = 0; i < 2; i++) {
        gfx_clear(COL_BG);
        gfx_text_center(GFX_W / 2, GFX_H / 2 - 40, line1, COL_TEXT);
        if (line2) {
            gfx_text_center(GFX_W / 2, GFX_H / 2 + 10, line2, COL_DIM);
        }
        present();
    }
}

/* 螢幕關著時停在這裡。回傳 false 代表卡片被拔掉。 */
static bool wait_screen_on(void)
{
    while (!screen_poll()) {
        watchdog_feed();
        if (!sd_present()) {
            return false;
        }
        HAL_Delay(100);
    }
    return true;
}

/* 判斷觸控是短按還是長按。回傳 0=沒碰到，1=短按，2=長按。
 *
 * 短按暫停／繼續、長按回選單。原本短按就直接離開，結果想停下來看某一張時
 * 只能被踢回選單，反而看不到。 */
#define LONG_PRESS_MS   700u

static int touch_gesture(void)
{
    uint32_t t0;
    int x, y;

    if (!touch_confirmed()) {
        return 0;
    }
    t0 = HAL_GetTick();
    while (read_touch(&x, &y)) {
        watchdog_feed();
        if (HAL_GetTick() - t0 >= LONG_PRESS_MS) {
            wait_release();
            return 2;               /* 長按：回選單 */
        }
        HAL_Delay(20);
    }
    wait_release();
    return 1;                       /* 短按：暫停／繼續 */
}

/* 暫停時的操作列版面（直立座標）。 */
#define BAR_Y       660
#define BAR_H       120
#define BAR_MID     (GFX_W / 2)

/* 操作列蓋住的照片區域備份在這裡，收起時原樣還原。
 * 480 列 x 120 行 x 2 bytes = 115KB，放在資料夾索引之後的空位。 */
#define BAR_SAVE    ((uint16_t *)0x91F00000u)

/* restore=false 備份、true 還原。操作列在邏輯 y=BAR_Y..BAR_Y+BAR_H，
 * 對應實體 framebuffer 每一列的第 BAR_Y~BAR_Y+BAR_H 行。 */
static void bar_backup(bool restore)
{
    uint16_t *front = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);

    for (uint32_t r = 0; r < PHYS_H; r++) {
        uint16_t *fb  = front + r * PHYS_W + BAR_Y;
        uint16_t *sav = BAR_SAVE + r * BAR_H;

        if (restore) {
            memcpy(fb, sav, BAR_H * sizeof(uint16_t));
        } else {
            memcpy(sav, fb, BAR_H * sizeof(uint16_t));
        }
    }
}

/* 把一塊區域壓暗，做出半透明的感覺。
 *
 * RGB565 沒有 alpha 通道，所以是讀回原像素、把亮度減半再寫回去。照片內容
 * 還看得見，但按鈕的字浮得出來。 */
static void overlay_dim(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint16_t v = gfx_get_pixel(x + i, y + j);
            uint16_t r = (uint16_t)((v >> 11) & 0x1Fu) >> 1;
            uint16_t g = (uint16_t)((v >> 5) & 0x3Fu) >> 1;
            uint16_t b = (uint16_t)(v & 0x1Fu) >> 1;
            gfx_pixel(x + i, y + j, (uint16_t)((r << 11) | (g << 5) | b));
        }
    }
}

/* 在「正在顯示」的那塊 buffer 上直接畫操作列。
 *
 * 不能畫在 back buffer —— 那塊還沒輪到顯示，畫了看不見。present() 之後
 * gfx 指向的是 back buffer，所以要先把它切到 front，畫完再切回來。 */
/* 四顆按鈕：上一張 | 繼續 | 下一張 | 選單。
 * 觸控判定用四等分（各 120 寬），比按鈕本體寬，好點。 */
#define BAR_BTN_W   105
#define BAR_BTN_GAP 12

static void draw_pause_bar(void)
{
    static const char *label[4] = { "上一張", "繼續", "下一張", "選單" };
    uint16_t *back  = gfx_framebuffer();
    uint16_t *front = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);

    gfx_set_framebuffer(front);

    overlay_dim(0, BAR_Y, GFX_W, BAR_H);

    for (int i = 0; i < 4; i++) {
        int x = BAR_BTN_GAP + i * (BAR_BTN_W + BAR_BTN_GAP);
        bool accent = (i == 1);                 /* 「繼續」高亮 */

        gfx_pill(x, BAR_Y + 24, BAR_BTN_W, BAR_H - 48,
                 accent ? COL_ACCENT : COL_PANEL);
        gfx_text_center(x + BAR_BTN_W / 2, BAR_Y + 44, label[i],
                        accent ? COL_BG : COL_TEXT);
    }

    gfx_set_framebuffer(back);
}

/* 暫停：畫面停在目前這張。
 * 回傳 PAUSE_RESUME / PAUSE_MENU / PAUSE_PREV / PAUSE_NEXT。 */
#define PAUSE_RESUME 0
#define PAUSE_MENU   1
#define PAUSE_PREV   2
#define PAUSE_NEXT   3
static int paused_loop(void)
{
    bool     bar_on;
    uint32_t bar_t0;

    g_paused = 1u;

    /* 先備份被列蓋住的區域，收起時才能原樣還原照片。 */
    bar_backup(false);
    draw_pause_bar();
    bar_on = true;
    bar_t0 = HAL_GetTick();

    for (;;) {
        int x, y;

        watchdog_feed();
        if (!sd_present()) {
            g_paused = 0u;
            return PAUSE_MENU;
        }
        if (!screen_poll()) {
            HAL_Delay(100);
            continue;
        }

        /* 列只停留一秒就收起，讓照片乾淨地顯示；再點一下隨時喚醒。 */
        if (bar_on && (HAL_GetTick() - bar_t0 > 1000u)) {
            bar_backup(true);
            bar_on = false;
        }

        if (read_touch(&x, &y)) {
            int x2, y2;

            /* 座標在確認的當下就抓，事後再讀手指已放開會撲空。 */
            HAL_Delay(20);
            if (read_touch(&x2, &y2)) {
                x = x2;
                y = y2;
            }
            wait_release();

            if (!bar_on) {
                /* 列已收起：這一下只負責喚醒，不觸發動作。 */
                bar_backup(false);
                draw_pause_bar();
                bar_on = true;
                bar_t0 = HAL_GetTick();
                continue;
            }

            if (y >= BAR_Y && y < BAR_Y + BAR_H) {
                bar_backup(true);       /* 還原照片再離開 */
                g_paused = 0u;
                if (x < GFX_W / 4)          { return PAUSE_PREV; }
                if (x < GFX_W / 2)          { return PAUSE_RESUME; }
                if (x < GFX_W * 3 / 4)      { return PAUSE_NEXT; }
                return PAUSE_MENU;
            }

            /* 點在列外：提前收起。 */
            bar_backup(true);
            bar_on = false;
        }
        HAL_Delay(30);
    }
}

/* 等目前這張照片的展示時間走完。
 *
 * already_ms 是「這張已經在螢幕上待了多久」—— 也就是解下一張花掉的時間。
 * 解碼期間前一張本來就在螢幕上，那段時間當然要算進展示時間裡，否則使用者
 * 設 2 秒會變成「解碼 1.5 秒 + 等 2 秒 = 3.5 秒」，設定值和實際對不上。
 *
 * 回傳 0 = 正常等完，1 = 要離開播放，2 = 卡片被拔掉，
 *     3 = 暫停中按了上一張，4 = 下一張。 */
static int wait_interval(uint32_t already_ms)
{
    uint32_t wait_ms = g_interval_s * 1000u;
    uint32_t waited  = (already_ms > wait_ms) ? wait_ms : already_ms;
    uint32_t last    = HAL_GetTick();

    while (waited < wait_ms) {
        uint32_t now;
        int g;

        watchdog_feed();
        if (!sd_present()) {
            return 2;
        }
        if (!screen_poll()) {
            HAL_Delay(100);
            last = HAL_GetTick();
            continue;
        }

        now = HAL_GetTick();
        waited += now - last;
        last = now;

        g = touch_gesture();
        if (g == 2) {
            g_exit_touch++;
            return 1;               /* 長按：回選單 */
        }
        if (g == 1) {
            int a = paused_loop();  /* 短按：暫停，停在這張 */
            if (a == PAUSE_MENU) { return 1; }
            if (a == PAUSE_PREV) { return 3; }
            if (a == PAUSE_NEXT) { return 4; }
            last = HAL_GetTick();   /* 暫停的時間不算進展示時間 */
        }
        HAL_Delay(15);
    }
    return 0;
}

/* 播放。
 *
 * 解碼與顯示重疊：目前這張在螢幕上的時候，就先把下一張解進另一塊 buffer，
 * 時間到直接切換。這樣換頁是瞬間的，而且間隔設定值就是實際的展示時間 ——
 * 只要解碼比間隔快。解碼比間隔慢的話，展示時間就等於解碼時間，沒辦法更快。 */
static void slideshow(void)
{
    uint32_t pos = 0;           /* 下一張要解碼的 g_order 索引 */
    int32_t  cur = -1;          /* 顯示中的索引，-1 = 還沒顯示過 */

    /* 先確定手指已經離開「開始播放」那一下。 */
    wait_release();

    for (;;) {
        photo_result_t r;
        uint32_t t0;

        watchdog_feed();
        if (!sd_present() || !wait_screen_on()) {
            return;
        }

        /* 解下一張：畫進 back buffer，此時前一張還在螢幕上。 */
        t0 = HAL_GetTick();
        r = photo_show(PLAYLIST_BASE + g_order[pos] * PATH_MAX);
        if (r != PHOTO_OK) {
            r = photo_show(PLAYLIST_BASE + g_order[pos] * PATH_MAX);
        }
        g_last_ms = HAL_GetTick() - t0;

        if (r == PHOTO_OK) {
            g_decode_ok++;

            if (cur >= 0) {
                int w = wait_interval(g_last_ms);
                if (w == 1 || w == 2) {
                    return;         /* 回選單或卡片被拔掉 */
                }
                if (w == 3 || w == 4) {
                    /* 暫停中的手動瀏覽：往指定方向同步解碼並顯示，
                     * 顯示完回到暫停狀態等下一個指令。back buffer 裡
                     * 預解好的那張作廢，恢復播放時重解。 */
                    int dir = (w == 3) ? -1 : 1;

                    for (;;) {
                        uint32_t target = (uint32_t)cur;
                        uint32_t tries  = 0;
                        photo_result_t br = PHOTO_ERR_READ;
                        int a;

                        /* 解不開就往同方向繼續找，最多繞一圈。 */
                        while (tries < g_order_count) {
                            watchdog_feed();
                            target = (target + g_order_count +
                                      (uint32_t)dir) % g_order_count;
                            br = photo_show(PLAYLIST_BASE +
                                            g_order[target] * PATH_MAX);
                            tries++;
                            if (br == PHOTO_OK) {
                                break;
                            }
                            g_decode_fail++;
                        }
                        if (br == PHOTO_OK) {
                            present();
                            cur = (int32_t)target;
                            g_decode_ok++;
                        }

                        a = paused_loop();
                        if (a == PAUSE_MENU) {
                            return;
                        }
                        if (a == PAUSE_RESUME) {
                            break;
                        }
                        dir = (a == PAUSE_PREV) ? -1 : 1;
                    }

                    pos = ((uint32_t)cur + 1u) % g_order_count;
                    continue;
                }
            }
            present();
            cur = (int32_t)pos;
        } else {
            /* 一張壞掉的照片不能讓相框停住，記錄之後直接換下一張。 */
            g_decode_fail++;
        }

        pos++;
        if (pos >= g_order_count) {
            build_order();              /* 播完一輪重新洗牌 */
            pos = 0;
        }
    }
}

/* ------------------------------------------------------------------ */

/* 掛載並掃描一次。回傳 false 代表這輪不能播（沒卡、掛不起來、沒照片）。 */
static bool mount_and_scan(void)
{
    g_top_count   = 0;
    g_photo_count = 0;
    g_order_count = 0;
    memset(g_top, 0, sizeof(g_top));

    g_stage = 31;
    g_err = (int32_t)f_mount(&g_fs, g_drive, 1);
    g_stage = 32;
    if (g_err != FR_OK) {
        show_message("記憶卡無法讀取", "請確認格式為 FAT32");
        return false;
    }

    g_stage = 4;
    strncpy(g_scan_path, g_drive, SCAN_PATH_LEN - 1U);
    g_scan_path[SCAN_PATH_LEN - 1U] = 0;
    {
        /* f_opendir 吃 "0:" 這種形式，結尾不要斜線。 */
        size_t n = strlen(g_scan_path);
        while (n > 0U && g_scan_path[n - 1U] == '/') {
            g_scan_path[--n] = 0;
        }
    }
    g_cur_top = MAX_TOP;
    scan(0);
    watchdog_feed();

    if (g_photo_count == 0U) {
        show_message("卡片裡沒有找到照片", "支援 jpg 與 jpeg");
        return false;
    }
    return true;
}

/* 卡片被拔掉之後的處理：卸載檔案系統，等使用者插回來。
 *
 * 一定要 f_mount(NULL) 卸載。FatFs 會把 FAT 表與目錄資訊快取在記憶體裡，
 * 換了一張卡還沿用舊快取，讀到的會是不存在的位置。 */
static void wait_for_card(void)
{
    f_mount(NULL, g_drive, 0);

    show_message("請插入記憶卡", "插入後會自動重新掃描");
    while (!sd_present()) {
        watchdog_feed();
        HAL_Delay(100);
    }

    /* 插入的瞬間接點會彈跳，等它穩定再動卡片，否則初始化容易失敗。 */
    for (int i = 0; i < 10; i++) {
        watchdog_feed();
        HAL_Delay(100);
    }
    show_message("讀取記憶卡", "載入中");
}

void album_run(void)
{
    TS_Init_t ts = { .Width = PHYS_W, .Height = PHYS_H,
                     .Orientation = TS_SWAP_NONE, .Accuracy = 2 };
    bool wdt_reset;

    g_stage = 1;

    /* 開啟週期計數器，rnd_mix() 拿它當高解析度的熵來源。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* 上一輪是被看門狗打掉的嗎？旗標要在這裡讀，之後會被清掉。 */
    wdt_reset = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != 0u);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    psram_mpu_init();
    screen_init();

    if (BSP_TS_Init(0, &ts) != BSP_ERROR_NONE) {
        show_message("觸控初始化失敗", 0);
    }
    button_init();

    g_stage = 2;
    if (!photo_init()) {
        show_message("JPEG 解碼器初始化失敗", 0);
        for (;;) { }
    }

    /* 插卡偵測腳要自己先設定好。
     *
     * BSP_SD_IsDetected() 只是讀 GPIO，不會設定它 —— 真正設定的是
     * BSP_SD_Init()。但主迴圈在掛載之前就會用 sd_present() 判斷有沒有卡，
     * 那時這支腳還沒被初始化過，讀到的是未定義狀態。實測沒插卡卻被判成
     * 有卡，於是走進掛載流程，最後顯示「記憶卡無法讀取，請確認格式為
     * FAT32」—— 訊息完全誤導。 */
    SD_DETECT_GPIO_CLK_ENABLE();
    {
        GPIO_InitTypeDef det = {0};
        det.Pin   = SD_DETECT_PIN;
        det.Mode  = GPIO_MODE_INPUT;
        det.Pull  = GPIO_NOPULL;
        det.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(SD_DETECT_GPIO_PORT, &det);
        HAL_Delay(10);              /* 讓腳位穩定再開始判斷 */
    }

    g_stage = 30;
    if (FATFS_LinkDriver(&SD_BSP_Driver, g_drive) != 0) {
        show_message("磁碟介面初始化失敗", 0);
        for (;;) { }
    }

    /* 被看門狗打掉，代表上一輪卡在記憶卡初始化裡出不來。這種狀態只有把卡片
     * 真正斷電（拔出來）才能清掉，所以先擋住不要再去碰它，否則會一直重置。 */
    if (wdt_reset) {
        show_message("記憶卡沒有回應", "請拔出記憶卡再重新插入");
        while (sd_present()) {
            HAL_Delay(100);
        }
    }

    watchdog_start();

    g_stage = 3;
    show_message("讀取記憶卡", "載入中");

    for (;;) {
        watchdog_feed();

        if (!sd_present()) {
            wait_for_card();
            continue;
        }
        if (!mount_and_scan()) {
            /* 掛不起來或沒照片：等使用者換一張卡再試。 */
            while (sd_present()) {
                watchdog_feed();
                HAL_Delay(200);
            }
            continue;
        }

#if DEBUG_ONE_SHOT
        /* 除錯：連續解好幾張（重現播放時的雙緩衝交替），最後停在一張上不動，
         * 方便用 SWD 把畫面完整抓下來。只解一張重現不了跨照片的問題。 */
        g_stage = 9;
        build_order();
        for (uint32_t k = 0; k < 6u && k < g_order_count; k++) {
            watchdog_feed();
            if (photo_show(PLAYLIST_BASE + g_order[k] * PATH_MAX) == PHOTO_OK) {
                g_decode_ok++;
                present();
            } else {
                g_decode_fail++;
            }
            HAL_Delay(300);
        }
        g_stage = 10;
        for (;;) {
            watchdog_feed();
            BSP_LED_Toggle(LD1);
            HAL_Delay(500);
        }
#else
        g_stage = 5;
        while (sd_present()) {
            if (select_screen()) {
                slideshow();
            }
        }
#endif
        /* 跳出來代表卡片被拔掉了。 */
    }
}
