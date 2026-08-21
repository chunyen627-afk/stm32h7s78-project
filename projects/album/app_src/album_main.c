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

#define MAX_TOP         24U
#define MAX_PHOTOS      4096U
#define PATH_MAX        128U
#define NAME_MAX        96U
#define MAX_DEPTH       10U
#define SCAN_PATH_LEN   512U

/* 清單放 PSRAM：4096 x 128 = 512KB，內部 RAM 放不下。 */
#define PLAYLIST_BASE   ((char *)0x91E00000u)
#define TOPIDX_BASE     ((uint8_t *)0x91E80000u)

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
    for (int i = 0; i < 3; i++) {
        HAL_Delay(20);
        if (!read_touch(&x, &y)) {
            return false;
        }
    }
    return true;
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
    if (on) {
        (void)BSP_LCD_DisplayOn(0);
    } else {
        (void)BSP_LCD_DisplayOff(0);
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

    if (g_photo_count >= MAX_PHOTOS || strlen(path) >= PATH_MAX) {
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

static void slideshow(void)
{
    uint32_t pos = 0;

    /* 進來時先確定手指已經離開「開始播放」那一下，再開始接受離開的觸碰。 */
    wait_release();

    for (;;) {
        const char *path = PLAYLIST_BASE + g_order[pos] * PATH_MAX;
        uint32_t t0 = HAL_GetTick();

        watchdog_feed();
        if (!sd_present()) {
            return;                     /* 卡片被拔掉，回到上層 */
        }

        /* 螢幕關著就停在這裡，不要開始解下一張。 */
        while (!screen_poll()) {
            watchdog_feed();
            if (!sd_present()) {
                return;
            }
            HAL_Delay(100);
        }

        photo_result_t r = photo_show(path);
        if (r != PHOTO_OK) {
            r = photo_show(path);      /* 偶發讀取失敗重試一次 */
        }
        if (r == PHOTO_OK) {
            g_decode_ok++;
            present();
        } else {
            /* 一張壞掉的照片不能讓相框停住，記錄之後直接換下一張。 */
            g_decode_fail++;
        }
        g_last_ms = HAL_GetTick() - t0;

        /* 等到間隔時間到；中途有明確的觸碰就回選擇畫面。
         *
         * 螢幕關著的時間不列入計算，所以再點亮時目前這張會有完整的展示時間，
         * 不會一亮起來就立刻跳下一張。 */
        {
            uint32_t wait_ms = g_interval_s * 1000u;
            uint32_t waited  = 0;
            uint32_t last    = HAL_GetTick();

            while (waited < wait_ms) {
                uint32_t now;

                watchdog_feed();
                if (!sd_present()) {
                    return;
                }

                if (!screen_poll()) {
                    /* 螢幕關著：停在這裡不解碼、不吃觸控，也不累計時間。
                     * 輪詢放慢到 100ms，減少不必要的耗電。 */
                    HAL_Delay(100);
                    last = HAL_GetTick();
                    continue;
                }

                now = HAL_GetTick();
                waited += now - last;
                last = now;

                if (touch_confirmed()) {
                    g_exit_touch++;
                    wait_release();
                    return;
                }
                HAL_Delay(15);
            }
        }

        pos++;
        if (pos >= g_order_count) {
            build_order();          /* 播完一輪重新洗牌 */
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

        g_stage = 5;
        while (sd_present()) {
            if (select_screen()) {
                slideshow();
            }
        }
        /* 跳出來代表卡片被拔掉了。 */
    }
}
