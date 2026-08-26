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
#include "video.h"
#include "favorites.h"
#include "vbus.h"
#include "usbaudio.h"
#include "usbdrive.h"
#include "audio_out.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

extern const Diskio_drvTypeDef SD_BSP_Driver;

/* 磁碟層在等卡片時會呼叫這個，讓長時間的等待不會被看門狗打掉。 */
void sd_bsp_set_keepalive(void (*fn)(void));

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
#define COL_HEART       RGB565(232, 72, 96)

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
static uint32_t g_interval_s = 2;      /* 預設 2 秒，可調 2~5 */

/* 播放順序。預設隨機 —— 相框是隨手看的，固定順序會一直看到同幾張。
 * 想照資料夾原本的順序（通常等於時間順序）就切成循序。 */
static bool g_shuffle = true;
static const char *const SEQ_NAME[2] = { "隨機", "循序" };

#define SCROLL_MIN_DY 24        /* 選單清單：拉多少才算捲動而不是點選 */
#define SWIPE_GAP_MS  150u      /* 容忍觸控中途漏報多久（board-notes 14.7）*/

/* 開機掃描完是否直接開播。
 *
 * 預設 0 = 停在選單，讓使用者確認要播哪些資料夾再開始（資料夾本來就
 * 預設全選，所以通常按一下「開始播放」就好）。
 *
 * 設 1 = 掃描完直接播，插電就會動。這是**除錯用**的：板子沒有 UART，
 * 每次驗證都要「燒錄 -> 手動勾資料夾 -> 按開始」，一個 session 重複十幾次
 * 而中間的手動步驟沒有任何除錯價值。要遠端自動驗證時用
 * `CFLAGS=-DAUTO_PLAY=1` 編一版即可。 */
#ifndef AUTO_PLAY
#define AUTO_PLAY 0
#endif

/* 診斷用，SWD 讀得到。 */
volatile uint32_t g_stage;
volatile int32_t  g_err;
volatile uint32_t g_decode_ok;
volatile uint32_t g_decode_fail;

/* 卡片「還插著但不再回應」的偵測。
 *
 * 寫入失敗會讓這張卡進 BUSY 就再也不回來，之後**連讀取都失敗**。
 * 那時每一張照片都解不開，而翻頁迴圈會把整份清單試一遍再重來 ——
 * 迴圈裡有乖乖餵狗，所以看門狗不會救，結果是永遠空轉：
 * 實測 g_decode_fail 衝到 1,511,732，使用者看到的就是「卡死」。
 *
 * 連續失敗超過門檻就判定卡片掛了，跳出去做一次完整的重新初始化。
 * 門檻要大於「偶爾一張壞檔」但遠小於整份清單。 */
#define CARD_SICK_RUN   24u
/* 連續幾張「解不開」就跳出來提示。
 *
 * **門檻要跟清單大小取小的那個。** 寫死 8 的話，只有一兩張照片的資料夾
 * 永遠湊不到，畫面還是什麼都不說 —— 使用者用「一張不支援的圖」測就踩到了。
 * 一張圖的清單失敗一次，就已經是「全部都不行」了。
 *
 * 反過來，大清單裡偶爾一兩張壞檔很正常，不該打斷播放，所以上限留 8。 */
#define BAD_PHOTO_RUN   8u

static uint32_t g_bad_photo_run;   /* 連續解不開幾張（卡片是好的）*/
static bool     g_bad_photo_told;  /* 這次播放已經提示過了 */
static uint32_t g_read_fail_run;
static bool     g_card_sick;

volatile uint32_t g_dbg_card_sick;      /* 判定過幾次 */
volatile uint32_t g_dbg_card_recover;   /* 重新初始化過幾次 */

/* **「卡片讀不到」與「照片解不開」要分開算。**
 *
 * 原本兩種都往同一個計數器加，24 次就判定卡片壞掉去做重新初始化。
 * 但照片解不開的時候卡片是好的 —— 於是恢復、再失敗、再恢復，無限循環，
 * 畫面看起來就是當機。使用者實際踩過：一個資料夾 66 張裡 64 張是
 * progressive JPEG（硬體只吃 baseline），整個相簿像卡死。
 *
 * 現在只有 PHOTO_ERR_READ 會累積到卡片壞掉的判定；解不開／太大另外算，
 * 由播放迴圈顯示訊息並回選單。 */
static void note_decode(photo_result_t r)
{
    if (r == PHOTO_OK) {
        g_decode_ok++;
        g_read_fail_run = 0;
        g_bad_photo_run = 0;
        return;
    }
    g_decode_fail++;

    if (r == PHOTO_ERR_READ) {
        if (++g_read_fail_run >= CARD_SICK_RUN && !g_card_sick) {
            g_card_sick = true;
            g_dbg_card_sick++;
        }
    } else {
        g_bad_photo_run++;   /* 卡片沒事，是照片本身 */
    }
}
volatile uint32_t g_last_ms;
volatile uint32_t g_exit_touch;   /* 因觸控離開播放的次數 */
volatile uint32_t g_skipped_long; /* 路徑太長被跳過的照片數 */
volatile uint32_t g_skipped_full; /* 清單滿了之後被跳過的照片數 */
volatile uint32_t g_paused;
/* 手感問題的診斷計數 */
volatile uint32_t g_dbg_btn_edge;    /* 按鍵中斷進來幾次 */
volatile uint32_t g_dbg_btn_short;   /* 判定為短按 */
volatile uint32_t g_dbg_btn_long;    /* 判定為長按 */
volatile uint32_t g_dbg_touch_hit;   /* 暫停中 read_touch 讀到觸碰幾次 */
volatile int32_t  g_dbg_swipe_dx;    /* 最後一次滑動的位移 */
volatile int32_t  g_dbg_swipe_dy;       /* 1 = 播放暫停中（畫面凍結）*/

/* ------------------------------------------------------------------ */
/* 基礎設施                                                            */
/* ------------------------------------------------------------------ */

/* PSRAM 分兩種快取政策，依「有沒有硬體會碰這塊記憶體」來切。
 *
 * 下半 16MB（0x90000000~0x90FFFFFF）維持 write-through，因為裡面有兩塊
 * 是 CPU 與硬體共用的：framebuffer 被 LTDC 每秒讀 60 次，JPEG 原始檔會被
 * JPEG 硬體讀走。write-through 保證 CPU 寫下去的內容立刻在記憶體裡。
 *
 * 上半 16MB（0x91000000~0x91FFFFFF）改成 write-back。這塊全部是 CPU 自己
 * 的暫存 —— RGB888 全尺寸、縮圖、播放清單、資料夾索引、圖示備份 ——
 * 沒有任何硬體會讀，所以不需要任何快取維護動作。
 *
 * 為什麼要分：write-through 在 ARMv7-M 是「不做 write-allocate」，而原本
 * 這裡還加上 NOT_BUFFERABLE，連寫入緩衝都關掉 —— 每一個 byte store 都變成
 * 一次獨立、同步、不能合併成 burst 的 PSRAM 交易。實測轉色每像素要 121 個
 * 週期（查表換算合理值是 15~25），銳化那段更高到 473，多出來的全是在等
 * 匯流排。改成 write-back 之後，連續的 byte store 會先在快取線裡累積，
 * 整條 32 bytes 一次 burst 出去。
 *
 * 重疊時編號大的優先，所以上半部用第 8 號蓋掉第 7 號那片 32MB。 */
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

    /* TEX=1 + C=1 + B=1 就是 write-back 且讀寫都 allocate。
     * write-allocate 是重點：寫入未命中時先把整條快取線讀進來，之後同一條線
     * 上的寫入全部命中，最後整條一次寫回，這樣才有 burst。 */
    mpu.Number           = MPU_REGION_NUMBER8;
    mpu.BaseAddress      = 0x91000000u;
    mpu.Size             = MPU_REGION_SIZE_16MB;
    mpu.IsCacheable      = MPU_ACCESS_CACHEABLE;
    mpu.IsBufferable     = MPU_ACCESS_BUFFERABLE;
    mpu.TypeExtField     = MPU_TEX_LEVEL1;
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

/* 黑盒子（重置洗不掉的觀察管道）。定義放在這裡是因為 present() 要用，
 * 完整的欄位說明與「為什麼放 DTCM」寫在下面的 bbox_boot()。 */
#define BBOX        ((volatile uint32_t *)0x20004020u)
#define BBOX_MAGIC  0x424F5831u   /* "BOX1" */

static void present(void)
{
    uint32_t drawn = g_front ? FB0_ADDR : FB1_ADDR;
    bool     reloaded = false;

    /* 只寫 CFBAR 一個暫存器：它是 shadow register，寫入不會立即生效，
     * 等垂直消隱重載時整批原子切換，中間沒有半完成狀態。 */
    LTDC_Layer1->CFBAR = drawn;
    LTDC->SRCR = LTDC_SRCR_VBR;
    for (uint32_t guard = 0; guard < 2000000u; guard++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) {
            reloaded = true;
            break;
        }
    }

    /* **等不到垂直消隱重載 = 面板沒在掃描 = 畫面凍住。**
     * 這個保險迴圈原本是靜靜地逾時然後往下走，於是「畫面不動但程式還活著」
     * 完全沒有跡象。數它一次就分得出「相簿卡住」與「相簿好好的、只是
     * 畫面停在最後一幀」—— 這兩件事在螢幕上長得一模一樣。 */
    BBOX[15]++;
    if (!reloaded) { BBOX[14]++; }

    g_front ^= 1u;
    gfx_set_framebuffer((uint16_t *)(g_front ? FB0_ADDR : FB1_ADDR));
}

/* 睡到下一個中斷才醒，用來取代輪詢迴圈裡的 HAL_Delay()。
 *
 * HAL_Delay() 是忙等 —— 核心全速空轉一直讀 uwTick，600MHz 的 M7 這樣燒電
 * 很不划算。相簿大部分時間都在等（每張顯示 2~5 秒、解碼只佔 1.5 秒），
 * 所以這裡是最值得省的地方。
 *
 * __WFI() 讓核心停住，直到有中斷才繼續。SysTick 每 1ms 一次，所以每毫秒
 * 醒一下、其餘時間都在睡，等待的精度不變但核心幾乎不耗電。
 *
 * 注意：中斷必須是開著的，否則會永遠睡下去。這裡是主迴圈，沒有關中斷的
 * 情況；如果哪天在臨界區裡呼叫就會卡死。 */
/* **開發期把睡眠關掉。**
 *
 * album_run() 裡已經有 HAL_DBGMCU_EnableDBGSleepMode()，理論上睡著也連得上
 * SWD。但這塊板子上實測不成立：相簿停在選單（每圈 nap(30)，幾乎整段時間
 * 都在睡）時，mode=HOTPLUG 連續 15 次全部連不上，報 "Unable to get core ID"；
 * 換頻率、換連線模式都一樣，只有 mode=UR（連線時按住重置，核心還來不及睡）
 * 連得上 —— 而 UR 會重置，等於把要觀察的狀態洗掉。
 *
 * 順帶量到的：`DBGMCU->CR` 讀出來是 0x10036485，**DBGCKEN（bit21）是 0**，
 * 而且從應用程式寫它也寫不進去。所以那個 HAL 呼叫沒有真的解決問題。
 *
 * 沒有 SWD 就沒有任何觀察手段（板子沒有 UART），音訊這一輪每一步都要靠
 * 讀全域變數驗證，所以開發期一律忙等。這台是插著電的相框，省電不是重點。 */
#define NAP_USE_WFI   0

static void nap(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < ms) {
        /* --- USB 無線耳機的列舉與類別請求就掛在這裡 ------------------
         * **相簿所有的迴圈都經過 nap()**（選單、看照片、播影片），
         * 所以這一個插入點就涵蓋全部，不必去改十幾個迴圈。
         *
         * 資料串流階段不靠這裡 —— 那時候相簿每格影片要 17ms，主迴圈
         * 餵不動 USB 的 1000 包/秒，會交給 TIM7 的中斷
         * （見 projects/usbaudio/README.md 的實測）。
         *
         * 這裡本來就是忙等（NAP_USE_WFI 是 0），順便做事不花額外時間。 */
        usbaudio_process();
#if NAP_USE_WFI
        __WFI();
#endif
    }
}

static void rnd_mix(uint32_t v);      /* 定義在下面的亂數區塊 */
static void watchdog_feed(void);      /* 定義在下面的記憶卡狀態區塊 */
static bool sd_present(void);
static void show_scan_progress(void);
static bool screen_poll(void);
static void show_message(const char *line1, const char *line2);
static void draw_name_clipped(int x, int y, const char *s, int limit,
                              uint16_t color);
static bool wav_path_of(const char *video_path, char *out, size_t cap);
static uint32_t autovideo_index(void);

/* 影片的音量（0~100）。之後控制列的 +/- 會改它 —— 先放一個明確的預設值，
 * 不做記憶體外的設定檔（使用者要的是「每次開機預設值就好」）。
 * 實測 40 太小聲（-24 dB），60 約 -14 dB。 */
static uint32_t g_audio_vol = 60u;

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
        nap(10);
    }
    nap(40);
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
/* ------------------------------------------------------------------ */
/* 黑盒子：重置洗不掉的觀察管道                                        */
/* ------------------------------------------------------------------ */
/**
 * 為什麼需要這個：SWD 的 mode=HOTPLUG 現在完全連不上（連續 15 次全失敗，
 * 報 "Unable to get core ID"），只有 mode=UR 連得上 —— 而 UR 會重置板子，
 * 等於把要觀察的狀態洗掉。全域變數在 .bss，開機就被清成 0，所以「重置之後
 * 再讀」讀到的永遠是新一輪的初值，看不到上一輪發生了什麼。
 *
 * 這塊放 DTCM。board-notes 22.5 已經實測過：bootloader 的堆疊在 AXI SRAM，
 * 完全不碰 DTCM，寫進去重置後原封不動（那一章就是靠這個發現「暗號被
 * bootloader 踩掉」的）。連結腳本給相簿的 DTCM 只有 0x20000000 起 4KB
 * （_estack = 0x20001000），所以 0x20004020 這裡沒有人用。
 *
 * 而且啟動碼不會清它 —— 它不是 .data 也不是 .bss。這正是重點：
 * **上一輪的紀錄活得過重置。**
 */
/* [0] 魔術字   [1] 開機次數（累計）
 * [2] 上一輪最後到達的 g_stage      [3] 上一輪最後一次餵狗的 uwTick
 * [4] 這一輪開機時的 RCC->RSR       [5] 這一輪的 g_stage（即時）
 * [6] 這一輪最後一次餵狗的 uwTick   [7] 餵狗次數（這一輪）
 * [8] mount_and_scan 進來幾次       [9] sd_present() 回報「沒卡」幾次
 * [10] sd_present() 總共問了幾次    [11] 最後一次掃完的 g_photo_count
 * [12] select_screen 進來幾次       [13] slideshow 進來幾次
 * [14] present() 等不到重載幾次     [15] present() 呼叫幾次
 *
 * [16] 音訊測試請求（Hz，外面寫、韌體吃掉）
 * [17..24] 音訊測試結果：step / init / pll / sel / play / half / full / err
 * [25] 測試跑完時的 uwTick
 *
 * **[16] 以後開機不清空。** 請求是在重置點寫進來的（那時韌體還沒跑），
 * 清掉就等於永遠收不到；結果則要活過下一次重置才讀得到。 */
static void bbox_boot(uint32_t rsr)
{
    if (BBOX[0] != BBOX_MAGIC) {    /* 斷電後第一次：內容是隨機的 */
        BBOX[0] = BBOX_MAGIC;
        BBOX[1] = 0u;
        BBOX[5] = 0u;
        BBOX[6] = 0u;
    }
    BBOX[1]++;                      /* 開機次數：一秒跳好幾次就是重置迴圈 */
    BBOX[2] = BBOX[5];              /* 把上一輪的即時值搬到「上一輪」欄 */
    BBOX[3] = BBOX[6];
    BBOX[4]  = rsr;
    BBOX[39] = BBOX[26];        /* 上一輪的開機腳印走到哪 */
    BBOX[26] = 0u;
    BBOX[5]  = 0u;
    BBOX[6]  = 0u;
    BBOX[7]  = 0u;
    BBOX[8]  = 0u;
    BBOX[9]  = 0u;
    BBOX[10] = 0u;
    BBOX[11] = 0u;
    BBOX[12] = 0u;
    BBOX[13] = 0u;
    BBOX[14] = 0u;
    BBOX[15] = 0u;
}

/* 把音訊的診斷變數抄進黑盒子。.bss 活不過重置，DTCM 活得過 ——
 * 而現在唯一能連上的 mode=UR 一定會重置，所以不抄就讀不到。 */
static void bbox_audio_snapshot(void)
{
    BBOX[17] = g_dbg_aud_step;
    BBOX[18] = g_dbg_aud_init;
    BBOX[19] = g_dbg_aud_pll;
    BBOX[20] = g_dbg_aud_sel;
    BBOX[21] = g_dbg_aud_play;
    BBOX[22] = g_dbg_aud_half;
    BBOX[23] = g_dbg_aud_full;
    BBOX[24] = g_dbg_aud_err;
    BBOX[25] = HAL_GetTick();

    /* 時脈的現場。音訊唯一動到的全域資源就是時脈樹（PLL3 + SPI6 選源），
     * 而 PSRAM(XSPI1) 與外部 NOR(XSPI2) 都吃 PLL2 —— 所以先確認 PLL2
     * 還在不在，再去碰任何一塊 PSRAM。順序很重要：這幾行都是讀暫存器，
     * 不碰 PSRAM，所以就算 PSRAM 已經壞了也記得下來。 */
    BBOX[32] = RCC->CR;          /* PLL1/2/3 的 ON 與 RDY 位元 */
    BBOX[33] = RCC->CKPROTR;     /* XSPI 的時脈保護有沒有跳到回復檔位 */
    BBOX[34] = RCC->CFGR;
    BBOX[35] = XSPI1->SR;

}

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
volatile uint32_t g_scan_ms;       /* 掃卡耗時，毫秒 */
volatile uint32_t g_usb_since;     /* VBUS 從何時開始持續有電 */
volatile uint32_t g_usb_pending;   /* VBUS 出現了，等安全點再切換 */

bool sd_bsp_write_unlocked(void);   /* 定義在 core/sd_bsp_diskio.c */

static void watchdog_feed(void)
{
    if (g_wdt_on) {
        (void)HAL_IWDG_Refresh(&g_iwdg);
    }

    /* 黑盒子的即時欄。watchdog_feed 是全域最常被呼叫的地方（所有可能久留的
     * 迴圈都會經過），所以這裡記的「最後一次」就是「它活到哪裡為止」。 */
    BBOX[5] = g_stage;
    BBOX[6] = HAL_GetTick();
    BBOX[7]++;

    /* 音訊的回呼計數要**持續**更新，不能只在測試當下拍一次快照 ——
     * DMA 有沒有真的在跑，看的就是這兩個數字會不會一直往上跳。
     * 「聽起來好像有聲音」不可靠，這個可靠。
     *
     * **只在這一輪真的跑過音訊時才更新。** 沒跑音訊的那一輪會用 0 把上一輪
     * 的數字蓋掉，於是「DMA 沒在跑」跟「數字被洗掉」長得一模一樣 ——
     * 這個量測污染實際害我誤判過一次。 */
    if (g_dbg_aud_step != 0u) {
        BBOX[22] = g_dbg_aud_half;
        BBOX[23] = g_dbg_aud_full;
        BBOX[24] = g_dbg_aud_err;
    }

    /* 影片的分項計時也鏡射一份：.bss 一重置就沒了，而唯一連得上的
     * mode=UR 一定會重置 —— 不鏡射就等於量不到。
     *
     * **同樣要擋掉沒播影片的那一輪**，否則它會用 0 把上一輪的數字蓋掉 ——
     * 這個坑我在音訊那邊已經踩過一次，影片這邊又踩了一次。
     * 「重置後那一輪會覆寫黑盒子」是這套機制的通病，每加一組欄位都要想。 */
    if (g_vdbg_decoded != 0u) {
        BBOX[120] = g_vdbg_decoded;
        BBOX[121] = g_vdbg_us_read;
        BBOX[122] = g_vdbg_us_dec;
        BBOX[123] = g_vdbg_us_cc;
        BBOX[124] = g_vdbg_fail;
    }
    if (g_dbg_wav_step != 0u) {
        BBOX[132] = g_dbg_wav_fed;      /* 串流了多少 -> 換算得出播了幾秒 */
        BBOX[133] = g_dbg_wav_under;
        BBOX[134] = g_dbg_wav_rderr;
        BBOX[135] = audio_wav_pos_ms();
        BBOX[138] = g_dbg_wav_us;
        BBOX[139] = g_dbg_wav_reads;
    }

    /* 順便偵測 USB 線插上沒。這裡只是讀一個暫存器（連續轉換模式一直在跑），
     * 而 watchdog_feed 本來就是全域最常被呼叫的地方 —— 不必另外找地方輪詢。
     *
     * **這裡只舉旗，不切換。** watchdog_feed 會從很深的地方被呼叫，包含
     * 正在寫「我的最愛」的當下 —— 在那裡重置會把清單檔寫壞。真正的切換
     * 放在主迴圈頂端，那裡保證不在寫卡。 */
    g_usb_pending = vbus_present() ? 1u : 0u;   /* 跟著 VBUS 走，不要單向鎖住 */

    /* 插上 USB 就讓位給隨身碟（不會返回）。
     *
     * **動作放在這裡，不是放在各個迴圈裡。** 先前只在播放迴圈與選單迴圈
     * 放檢查點，結果相簿停在第三種狀態時就不會切換 —— 一個一個補是打地鼠。
     * watchdog_feed 是全域最常被呼叫的地方，涵蓋所有狀態。
     *
     * 唯一不能重置的時機是正在寫卡（會把「我的最愛」寫壞）。直接問既有的
     * 寫入鎖，不另外發明一個要同步的旗標。 */
    /* **要連續一秒讀到才算數。** 原本是讀到一次就切，但切過去是單向的
     * （隨身碟 app 沒有自動回程），所以任何一次誤判都會黏住 ——
     * 症狀是「拔掉線按 reset 回不來」。最可能的誤判來源是拔線瞬間
     * 線材與板子上的電容還殘留著 5V，這時開機就會讀到「有電」。 */
    if (g_usb_pending && !sd_bsp_write_unlocked()) {
        uint32_t now = HAL_GetTick();

        if (g_usb_since == 0u) { g_usb_since = now; }
        else if ((now - g_usb_since) > 1000u) { usbdrive_request_switch(); }
    } else {
        g_usb_since = 0u;
    }
}

static bool sd_present(void)
{
    bool present = (BSP_SD_IsDetected(0) == SD_PRESENT);

    /* 這支腳一旦跳一下，播放/選單迴圈就會跳出去重新掛載並重掃 ——
     * 螢幕上看起來就是「一直停在載入中」。所以要數它，不要只用它。 */
    BBOX[10]++;
    if (!present) { BBOX[9]++; }
    return present;
}

/* ------------------------------------------------------------------ */
/* USER 按鈕：螢幕開關                                                 */
/* ------------------------------------------------------------------ */

/* 亮度分檔。人眼對亮度的感知接近對數，線性分檔（100/75/50/25）看起來
 * 差別很小 —— 25% 佔空比看起來大約還有一半亮。所以改成感知上等距的級距。 */
static void brightness_set(uint32_t pct);   /* 定義在下面，中斷會直接呼叫 */

static const uint8_t BRIGHT_STEP[] = { 100u, 50u, 20u, 6u, 0u };
#define BRIGHT_STEPS  (sizeof(BRIGHT_STEP) / sizeof(BRIGHT_STEP[0]))

static uint32_t         g_bright_idx;      /* 指向 BRIGHT_STEP */
static uint32_t         g_brightness = 100u;
static bool             g_screen_on  = true;

/* 觸控維持輪詢，不要用 BSP_TS_EnableIT()。
 *
 * 踩過的雷：啟用觸控中斷之後，ISR 只清了 EXTI 的旗標，沒有去讀 GT911 的
 * 狀態暫存器。控制器的資料沒被取走就不再回報新的觸碰，於是
 * BSP_TS_GetState() 之後永遠回傳「沒碰到」—— 症狀是觸控整個失效。
 *
 * 要正確使用中斷，回呼裡必須把資料讀走（等於還是要做一次 I2C 交易），
 * 省不到什麼。而且現在的控制是按鍵負責、觸控只在暫停時判滑動，輪詢完全
 * 夠用。 */



/* USER 鍵三段手勢，全部在中斷裡判斷：
 *
 *   短按 (<600ms)    切換背光亮度（立刻套用）
 *   長按 (>=600ms)   暫停 / 繼續播放
 *
 * 沒有「更長按」這一段：使用者反映很容易不小心按過頭，本來只想暫停卻跳回
 * 選單。回選單改由暫停中往下滑提供 —— 用時間長度區分三段操作，對手指來說
 * 太難拿捏。
 *
 * BSP 預設是 GPIO_MODE_IT_FALLING（下拉、按下為高），中斷只在放開時觸發，
 * 量不出按了多久。這裡改成雙邊緣，在中斷裡讀腳位判斷是按下還是放開：
 * 按下記時間、放開算長度再分派。
 *
 * 亮度直接在中斷裡套用（只寫 GPIO 與 LPTIM 暫存器，很快）；暫停與回選單
 * 要主迴圈配合，所以立旗標，並由 photo_show() 的中斷檢查即時放棄解碼 ——
 * 否則按下去要等解碼那 1.5 秒跑完才有反應。 */
#define PRESS_LONG_MS   600u

static volatile uint8_t  g_req_pause;
static volatile uint32_t g_press_t0;
static volatile uint8_t  g_pressing;

void EXTI13_IRQHandler(void)
{
    BSP_PB_IRQHandler(BUTTON_USER);
}

void BSP_PB_Callback(Button_TypeDef Button)
{
    uint32_t now = HAL_GetTick();
    uint32_t held;

    if (Button != BUTTON_USER) {
        return;
    }
    g_dbg_btn_edge++;

    if (HAL_GPIO_ReadPin(BUTTON_USER_GPIO_PORT, BUTTON_USER_PIN)
        == GPIO_PIN_SET) {
        if (!g_pressing) {
            g_pressing = 1u;
            g_press_t0 = now;
        }
        return;
    }

    if (!g_pressing) {
        return;                     /* 沒配對到的放開，忽略 */
    }
    g_pressing = 0u;

    held = now - g_press_t0;
    if (held < 30u) {
        return;                     /* 太短，是彈跳 */
    }
    if (held >= PRESS_LONG_MS) {
        g_dbg_btn_long++;
        g_req_pause = 1u;
    } else {
        g_dbg_btn_short++;
        g_bright_idx = (g_bright_idx + 1u) % BRIGHT_STEPS;
        brightness_set(BRIGHT_STEP[g_bright_idx]);
    }
}

/* 給 photo.c：有待處理的按鍵指令就別把這張解完了。 */
static bool ctrl_waiting(void)
{
    return (g_req_pause != 0u);
}

static void button_init(void)
{
    GPIO_InitTypeDef btn = {0};

    LCD_BL_CTRL_GPIO_CLK_ENABLE();      /* brightness_set 從中斷呼叫，不碰 RCC */
    (void)BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

    /* BSP 只設下降邊緣（放開才觸發），量不出按了多久。改成雙邊緣。 */
    BUTTON_USER_GPIO_CLK_ENABLE();
    btn.Pin   = BUTTON_USER_PIN;
    btn.Mode  = GPIO_MODE_IT_RISING_FALLING;
    btn.Pull  = GPIO_PULLDOWN;
    btn.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(BUTTON_USER_GPIO_PORT, &btn);
}

/* 設定背光亮度（0~100）。0 等同關螢幕，播放也會一起暫停。
 *
 * 全程只呼叫 BSP_LCD_SetBrightness()，「不碰」BSP_LCD_DisplayOn/Off ——
 * 那兩支是 ST 的不對稱陷阱：DisplayOff 會把背光腳（GPIOG15）從 LPTIM 的
 * PWM 替代功能改成一般 GPIO 並拉低，DisplayOn 卻完全不碰它。結果是
 * 「關得掉、開不回來」，而且腳位一旦離開 AF 模式，亮度調節就整個失效。
 *
 * SetBrightness 只改 LPTIM 的比較值（佔空比），那支腳從頭到尾維持 AF，
 * 所以 0% 就是 PWM 歸零＝背光全滅，100% 就是全亮，中間任意檔位都可用。 */
static void brightness_set(uint32_t pct)
{
    GPIO_InitTypeDef bl = {0};

    g_brightness = pct;
    g_screen_on  = (pct > 0u);

    /* 這支函式會從中斷裡被呼叫，所以不碰 RCC —— 時脈在 button_init() 開好。
     * 剩下的只有 GPIO 與 LPTIM 的暫存器寫入，沒有其他人同時在動這兩個周邊。 */
    bl.Pull  = GPIO_NOPULL;
    bl.Speed = GPIO_SPEED_FREQ_MEDIUM;
    bl.Pin   = LCD_BL_CTRL_PIN;

    if (pct == 0u) {
        /* BSP 的亮度公式在 0 會整數下溢：
         *     Pulse = (10001 * 0 / 100) - 1 = 0 - 1 -> 0xFFFFFFFF
         * 比較值變成天文數字，PWM 永遠不翻轉，背光根本關不掉。
         * 所以 0% 不走 PWM，直接把腳位切成一般輸出並拉低。 */
        bl.Mode = GPIO_MODE_OUTPUT_PP;
        HAL_GPIO_Init(LCD_BL_CTRL_GPIO_PORT, &bl);
        HAL_GPIO_WritePin(LCD_BL_CTRL_GPIO_PORT, LCD_BL_CTRL_PIN,
                          GPIO_PIN_RESET);
        return;
    }

    /* 從 0% 回來時腳位還停在一般輸出，要先還原成 LPTIM 的替代功能，
     * 否則 SetBrightness 改了比較值也沒有用（腳位不歸 LPTIM 管）。 */
    bl.Mode      = GPIO_MODE_AF_PP;
    bl.Alternate = LCD_LPTIMx_CHANNEL_AF;
    HAL_GPIO_Init(LCD_BL_CTRL_GPIO_PORT, &bl);

    (void)BSP_LCD_SetBrightness(0, pct);
}

/* 放在每個迴圈裡。回傳 true 代表螢幕是亮的（可以接受觸控）。
 * USER 鍵每按一次降一檔，到 0 之後回到 100。 */
/* 亮度已經在中斷裡套用了，這裡只回報「螢幕是不是亮的」，
 * 決定要不要繼續解碼與接受觸控。 */
static bool screen_poll(void)
{
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
/* true = 播放清單來自最愛，不是勾選的資料夾。
 *
 * 一定要是**狀態**而不是「建一次就好」：播完一輪會再呼叫 build_order()
 * 重洗牌，沒有這個旗標的話播完最愛就會偷偷跳回全部資料夾。 */
static bool g_fav_mode;

static void build_order(void)
{
    g_order_count = 0;
    for (uint32_t i = 0; i < g_photo_count; i++) {
        if ((i & 0xFFu) == 0u) {
            watchdog_feed();    /* 最愛模式每張都要比對，四千張要一點時間 */
        }
        if (g_fav_mode) {
            if (!fav_is(PLAYLIST_BASE + i * PATH_MAX)) { continue; }
        } else {
            uint32_t t = TOPIDX_BASE[i];
            if (!(t < g_top_count && g_top[t].selected)) { continue; }
        }
        g_order[g_order_count++] = (uint16_t)i;
    }

    if (!g_shuffle) {
        return;                     /* 循序：維持掃描到的順序 */
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
/* 影片                                                                */
/* ------------------------------------------------------------------ */

#define MAX_VIDEOS      16U

/* 影片清單只掃根目錄。
 *
 * 影格包動輒上百 MB，放根目錄是自然的擺法；而遞迴掃描每個 .bin 都要開檔
 * 讀檔頭，深層目錄裡如果有一堆無關的 .bin 會拖很久。照片那邊需要遞迴是
 * 因為使用者的相簿本來就有目錄結構，影片沒有這個需求。 */
static video_info_t g_vids[MAX_VIDEOS];

/* 每部影片上次看到哪一格。按「返回」離開時記下來，下次進去接著看。
 *
 * 用陣列而不是只記最後一部：卡上會有好幾部，看到一半跳去看別部再回來
 * 是很自然的用法，只記一部的話那個情境就失效了 —— 而成本只差 64 bytes。
 *
 * 索引跟 g_vids 一樣，所以**重新掃卡之後就不算數了**（順序可能變）。
 * 掃卡會把它清掉，寧可從頭播也不要接到別部影片的位置。 */
static uint32_t g_vid_resume[MAX_VIDEOS];
static uint32_t     g_vid_count;
/* --- 除錯建置：自動播影片 + 自動滑動 ----------------------------------
 * 非 0 就進入全自動測試：開機直接播第一部有音軌的影片，然後每隔這麼多
 * 毫秒自動滑動一次。**走的是跟觸控滑動完全一樣的路徑**，只是不用人在
 * 旁邊點。查 USB 音訊那個「滑一兩下就當」用的 —— 一輪要燒錄、重現、
 * 讀黑盒子，每一輪都要人配合就慢得離譜（board-notes 16.7 同一條教訓）。
 *
 * **正式建置一定要是 0。** */
#define ALBUM_AUTOPLAY      0     /* 開機直接播第一部有音軌的影片 */
#define ALBUM_AUTOSEEK_MS   0u     /* 非 0 就每隔這麼多毫秒自動滑動一次 */

/* 二分法：0 = 完全不啟動 USB 主機（連 usbaudio_init 都不呼叫）。
 * 用來判定「自動滑動會當」到底是不是 USB 帶進來的。 */
#define ALBUM_USB_AUDIO     1

/* 快取二分：1 = 進相簿就關掉 D-Cache。判定「USB 一開就卡死 SD 讀取」
 * 是不是快取／推測式存取這一類（usbaudio README「新方向」的第 3 步）。
 * 只關資料快取，I-Cache 照開。**正式建置一定要是 0。** */
#define ALBUM_DCACHE_OFF    0

volatile uint32_t   g_dbg_autovideo;   /* SWD 可寫，見主迴圈 */
volatile uint32_t   g_dbg_audiotest;   /* SWD 寫入頻率（Hz）就播測試音 */
volatile uint32_t   g_dbg_autoplay;    /* SWD 可寫，直接開始放照片 */
volatile uint32_t   g_dbg_fakedirs;    /* SWD 可寫，測試資料夾清單捲動 */
volatile uint32_t   g_dbg_fakevids;    /* SWD 可寫，測試影片清單捲動 */
volatile uint32_t   g_dbg_favtest;     /* SWD 可寫，見 fav_selftest() */
volatile uint32_t   g_dbg_wrtest;      /* SWD 可寫，純寫入壓力測試 fav_stress() */
volatile uint32_t   g_dbg_cardreinit;  /* SWD 可寫，重新初始化卡片（換時脈用）*/

static bool ends_with_bin(const char *name)
{
    size_t n = strlen(name);

    if (n < 5u) {
        return false;
    }
    return (name[n - 4] == '.') &&
           (name[n - 3] == 'b' || name[n - 3] == 'B') &&
           (name[n - 2] == 'i' || name[n - 2] == 'I') &&
           (name[n - 1] == 'n' || name[n - 1] == 'N');
}

static void scan_videos(void)
{
    DIR     dir;
    FILINFO fno;

    g_vid_count = 0;
    if (f_opendir(&dir, "0:") != FR_OK) {
        return;
    }
    while (g_vid_count < MAX_VIDEOS) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) {
            break;
        }
        if ((fno.fattrib & AM_DIR) || !ends_with_bin(fno.fname)) {
            continue;
        }
        watchdog_feed();

        {
            video_info_t *v = &g_vids[g_vid_count];
            size_t        n = strlen(fno.fname);

            /* 名字太長就跳過，不要讓 snprintf 默默截斷 —— 截斷後的路徑會
             * 指到別的檔案或根本開不起來，症狀比「這部影片沒出現」難查得多。 */
            if ((n + 4u) > VIDEO_PATH_MAX || (n + 1u) > VIDEO_NAME_MAX) {
                continue;
            }
            /* 路徑要自己組："0:" 加斜線加檔名。 */
            memcpy(v->path, "0:/", 3u);
            memcpy(v->path + 3, fno.fname, n + 1u);
            /* video_probe 只讀 24 bytes 的檔頭，不是有效的影格包就跳過 ——
             * 卡上可能有各種無關的 .bin。 */
            if (!video_probe(v->path, v)) {
                continue;
            }
            /* 長度上面已經檢查過，memcpy 比 snprintf 直接（也不會讓
             * 編譯器因為推不出上界而一直警告可能截斷）。 */
            memcpy(v->name, fno.fname, n + 1u);
            g_vid_count++;
        }
    }
    (void)f_closedir(&dir);
}

/* ---- 播放中的疊加層 ---------------------------------------------- */

#define OV_MS        3000U     /* 觸碰後疊加層顯示多久 */
#define PB_X         40
#define PB_W         400
#define PB_Y         700
#define PB_H         14
#define PB_HIT       60        /* 進度條上下各留這麼多的觸控範圍 */
#define BACK_X       20
#define BACK_Y       36
#define BACK_W       120
#define BACK_H       56
/* 音量列。放在進度條上方、跟它切齊 —— 使用者叫出控制列之後手指本來就在
 * 畫面下半部，不必再往上跑。按鈕給 90x64，比返回鈕還大：這是要在
 * 影片播放中盲按的東西。 */
#define VOL_Y        608
#define VOL_H        64
#define VOL_BTN_W    90
#define VOL_MINUS_X  PB_X
#define VOL_PLUS_X   (PB_X + PB_W - VOL_BTN_W)
#define VOL_STEP     10u

static bool g_vid_has_audio;   /* 這一部有沒有音軌（沒有就不畫音量列）*/

static void draw_time(int x, int y, uint32_t secs, uint16_t col)
{
    gfx_number(x, y, secs / 60u, col);
    gfx_text(x + (secs / 60u >= 10u ? 24 : 12), y, ":", col);
    {
        uint32_t s = secs % 60u;
        int      tx = x + (secs / 60u >= 10u ? 34 : 22);

        if (s < 10u) {
            gfx_number(tx, y, 0, col);
            gfx_number(tx + 12, y, s, col);
        } else {
            gfx_number(tx, y, s, col);
        }
    }
}

/* 疊加層每格都要重畫 —— DMA2D 轉色會把整個 framebuffer 蓋掉。
 * 所以只在使用者剛碰過螢幕的幾秒內畫，平常不花這個成本。 */
static void draw_overlay(const video_info_t *v, uint32_t idx, bool paused)
{
    uint32_t done_w = (uint32_t)PB_W * idx / (v->count ? v->count : 1u);
    uint32_t fps100 = v->fps_x100 ? v->fps_x100 : 2400u;

    gfx_pill(BACK_X, BACK_Y, BACK_W, BACK_H, COL_PANEL);
    gfx_text_center(BACK_X + BACK_W / 2, BACK_Y + 16, "返回", COL_TEXT);

    if (paused) {
        gfx_pill(GFX_W - 140, BACK_Y, BACK_W, BACK_H, COL_PANEL);
        gfx_text_center(GFX_W - 140 + BACK_W / 2, BACK_Y + 16, "暫停", COL_ACCENT);
    }

    /* 進度條：底槽 + 已播進度 + 目前位置的把手。 */
    gfx_fill_rect(PB_X, PB_Y, PB_W, PB_H, COL_LINE);
    if (done_w) {
        gfx_fill_rect(PB_X, PB_Y, (int)done_w, PB_H, COL_ACCENT);
    }
    gfx_fill_rect(PB_X + (int)done_w - 3, PB_Y - 8, 6, PB_H + 16, COL_TEXT);

    draw_time(PB_X, PB_Y + 30, (uint32_t)((uint64_t)idx * 100u / fps100),
              COL_TEXT);
    draw_time(PB_X + PB_W - 60, PB_Y + 30,
              (uint32_t)((uint64_t)v->count * 100u / fps100), COL_DIM);

    /* 音量列只在這部片真的有音軌時出現。沒有音軌卻畫一排音量鍵，
     * 就是 18.3 那條「畫面上寫著功能、程式裡沒有做」的反面 ——
     * 按了不會有任何事發生，使用者會以為壞了。 */
    if (g_vid_has_audio) {
        char buf[24];

        gfx_pill(VOL_MINUS_X, VOL_Y, VOL_BTN_W, VOL_H, COL_PANEL);
        gfx_text_center(VOL_MINUS_X + VOL_BTN_W / 2, VOL_Y + 20, "－", COL_TEXT);

        gfx_pill(VOL_PLUS_X, VOL_Y, VOL_BTN_W, VOL_H, COL_PANEL);
        gfx_text_center(VOL_PLUS_X + VOL_BTN_W / 2, VOL_Y + 20, "＋", COL_TEXT);

        if (g_audio_vol == 0u) {
            (void)snprintf(buf, sizeof(buf), "靜音");
        } else {
            (void)snprintf(buf, sizeof(buf), "音量 %u", (unsigned)g_audio_vol);
        }
        gfx_text_center(GFX_W / 2, VOL_Y + 20, buf,
                        (g_audio_vol == 0u) ? COL_DIM : COL_ACCENT);
    }
}

/* 播放一部影片，無限循環，直到使用者按返回或卡片被拔掉。 */
static void play_video(const video_info_t *v)
{
    uint32_t vi  = (uint32_t)(v - g_vids);
    uint32_t idx = 0, next_ms, acc = 0;
    uint32_t frame_us, frame_ms, frame_frac;
    uint32_t ov_until = 0;
    uint32_t vol_lock = 0u;   /* 按過音量之後要連續幾次讀不到觸控才解鎖 */
    int32_t  dmax = -1000000, dmin = 1000000;   /* 音畫偏差的極值 */
    bool     paused = false;
    bool     dcache_off = false;   /* 本次播放有沒有把 D-Cache 關掉 */

    if (!video_open(v)) {
        show_message("影片開啟失敗", v->name);
        nap(1500);
        return;
    }

    /* 接續上次看到的位置。
     *
     * **快看完了就從頭播。** 停在最後幾秒的話「接續」等於一進去就結束，
     * 使用者要的顯然是重看一次 —— 這種邊界不處理的話，最後那幾秒會變成
     * 一個進不去的狀態。門檻取十秒。 */
    if (vi < MAX_VIDEOS && g_vid_resume[vi] < v->count) {
        uint32_t tail = (uint32_t)((uint64_t)v->fps_x100 * 10u / 100u);

        if (g_vid_resume[vi] + tail < v->count) {
            idx = g_vid_resume[vi];
        }
        BBOX[146] = idx;            /* 這次從第幾格開始 */
    }

    /* 開播前的音訊預填就會讀 SD，所以危險組態的判斷要在這之前做一次；
     * 迴圈頂那個檢查負責接住「播到一半才插 dongle」（說明見迴圈頂）。 */
    if (usbaudio_session_active()) {
        SCB_DisableDCache();
        dcache_off = true;
    }

    /* 有同名的 .wav 就一起播。沒有就照舊只放畫面 —— 卡上八部影片只有一部
     * 有音軌，音訊不該變成播放的前提。 */
    {
        char wav[PATH_MAX];
        bool has = wav_path_of(v->path, wav, sizeof(wav));

        BBOX[136] = HAL_GetTick();   /* 開始播的時刻。沒有它就只能用推的，
                                      * 而我剛才就推錯了一次。 */
        g_vid_has_audio = has;
        BBOX[128] = has ? 1u : 0u;
        BBOX[129] = has ? (audio_wav_start(wav, g_audio_vol) ? 1u : 0u) : 0xFFu;
        BBOX[130] = g_dbg_wav_step;
        BBOX[131] = g_audio_vol;
    }

    /* 疊加層用直立座標畫，跟選單一致 —— 影格本身已經在 PC 上轉成
     * 「直立看正確」的方向，所以兩者的上下左右是同一套。 */
    gfx_set_orientation(false);

    frame_us   = 100000000u / (v->fps_x100 ? v->fps_x100 : 2400u);
    /* 聲音也要跳到同一個位置，否則接續播放的第一秒就對不上。 */
    if (idx != 0u) {
        (void)audio_wav_seek_ms((uint32_t)((uint64_t)idx * frame_us / 1000u));
    }
    frame_ms   = frame_us / 1000u;
    frame_frac = frame_us % 1000u;
    next_ms    = HAL_GetTick();

    for (;;) {
        int  x, y;
        bool touched;

        watchdog_feed();

        /* --- USB 會話活躍時，影片播放全程關 D-Cache -------------------
         *
         * 2026-08-27 A/B/A 實測收斂出來的結論：USB 主機會話一活躍
         * （dongle 連上、埠有電），加上 D-Cache 的匯流排交易，再加上
         * 影片這種 SD 持續串流＋滑動，幾秒內就會把匯流排互連整個鎖死
         * —— 主迴圈與所有中斷同瞬間停、沒有任何 CPU 故障、連 SWD 的
         * AHB-AP 都進不去，只剩看門狗能救。卡死的位置用 DTCM 探針
         * （UBOX 188/189）抓過三次：OTG 暫存器讀取一次、SDMMC 的
         * f_read 兩次 —— CPU 死在「當下剛好碰到的從屬」上，代表死的
         * 是互連，不是某個週邊。
         *
         * 排除掉的（都實測過，不要重試）：GAHBCFG.GINT（設了清、
         * 從頭不設都一樣死）、NVIC 的 OTG 中斷（本來就關）、CRS 修整
         * （收斂後凍結 TRIM 照樣死）、SDMMC 降頻（1/2 與 1/8 都沒用）。
         * 唯一實證有效的是關 D-Cache：同一份自動滑動治具從「7~12 秒
         * 必死」變成「209 秒、51 次滑動零異常」，而且影片照樣跑滿
         * 23.6fps（解碼在硬體 JPEG＋DMA2D 上，CPU 快取影響小）。
         *
         * 所以把代價限縮到最小：只在「USB 會話活躍」時、只在影片播放
         * 期間關。照片瀏覽（快取效益最大的路徑）不受影響；沒插 dongle
         * 時影片也維持原本的組態。放在迴圈頂而不是進場，是為了接住
         * 「播到一半才插 dongle」。 */
        if (!dcache_off && usbaudio_session_active()) {
            SCB_DisableDCache();
            dcache_off = true;
        }

        audio_wav_pump();
        if (!sd_present()) {
            break;
        }

        BBOX[149] = 1u;               /* 麵包屑：迴圈頂 */
        if (!paused) {
            uint8_t *back = (uint8_t *)(g_front ? FB0_ADDR : FB1_ADDR);

            if (video_decode(idx, back)) {
                if (HAL_GetTick() < ov_until) {
                    draw_overlay(v, idx, false);
                }
                present();
            }

            /* **逐格量音畫偏差，記極值。**
             *
             * 只在結束時取樣一次是不夠的 —— 那看不出它是「一直在那個值
             * 附近」還是「一路往一個方向爬」，而這兩件事的意義完全不同：
             * 前者是固定偏移（無害），後者才是漂移（會越來越糟）。
             *
             * 影格時間用 fps_x100 反算，跟播放節奏用的是同一個來源。 */
            if (g_vid_has_audio) {
                /* 用 frame_us 而不是重算一次 —— 這樣比對的正是播放節奏
                 * 實際使用的那個數字（含它的整數截斷），量到的就是真正
                 * 會發生在畫面上的偏差。 */
                int32_t vms = (int32_t)((uint64_t)idx * frame_us / 1000u);
                int32_t d   = (int32_t)audio_wav_pos_ms() - vms;

                if (d > dmax) { dmax = d; }
                if (d < dmin) { dmin = d; }
                BBOX[142] = (uint32_t)dmax;
                BBOX[143] = (uint32_t)dmin;
                BBOX[144] = (uint32_t)d;

                /* 開頭的基準值：跑到第 240 格（約十秒，啟動的暫態過去了）
                 * 記一次。有了它就能直接算「後來 - 開頭」= 真正累積的漂移，
                 * 不必從 max/min 的包絡去猜 —— 包絡裡混著影格量化的
                 * 一個週期，那不是漂移。 */
                if (idx == 240u) { BBOX[145] = (uint32_t)d; }
            }
            BBOX[149] = 2u;           /* 解碼與 present 都做完了 */
            idx = (idx + 1u) % v->count;      /* 無限循環 */
        }

#if ALBUM_AUTOSEEK_MS
        /* 自動滑動：跟下面觸控那條路做一模一樣的事（改 idx、跳音訊、
         * 重新對時），只是由時間觸發。位置用一個不會重複的序列走遍整條
         * 進度條，免得每次都滑到同一個地方而漏掉某些情況。 */
        {
            static uint32_t as_next;
            static uint32_t as_seq;

            if ((int32_t)(HAL_GetTick() - as_next) >= 0) {
                uint32_t px = (as_seq * 137u) % (uint32_t)PB_W;

                as_seq++;
                idx = (uint32_t)((uint64_t)px * v->count / PB_W);
                if (idx >= v->count) { idx = v->count - 1u; }
                BBOX[149] = 8u;           /* 自動滑動：seek 之前 */
                (void)audio_wav_seek_ms((uint32_t)((uint64_t)idx * 100000u /
                                        (v->fps_x100 ? v->fps_x100 : 2400u)));
                BBOX[149] = 9u;           /* 自動滑動：seek 回來了 */
                next_ms = HAL_GetTick();
                acc     = 0;
                as_next = HAL_GetTick() + ALBUM_AUTOSEEK_MS;
                BBOX[146] = as_seq;          /* 自動滑了幾次 */
            }
        }
#endif

        BBOX[149] = 3u;               /* 自動滑動那一段過了 */
        touched = read_touch(&x, &y);
        if (!touched && vol_lock > 0u) { vol_lock--; }
        if (touched) {
            if (HAL_GetTick() >= ov_until) {
                /* 第一次碰只叫出疊加層，不會誤觸到底下的按鈕。 */
                ov_until = HAL_GetTick() + OV_MS;
            } else if (x >= BACK_X && x < BACK_X + BACK_W &&
                       y >= BACK_Y && y < BACK_Y + BACK_H) {
                g_bad_photo_run = 0;   /* 每次開始播放重新算 */
    g_bad_photo_told = false;
    wait_release();
                break;
            } else if (g_vid_has_audio &&
                       y >= VOL_Y && y < VOL_Y + VOL_H &&
                       ((x >= VOL_MINUS_X && x < VOL_MINUS_X + VOL_BTN_W) ||
                        (x >= VOL_PLUS_X  && x < VOL_PLUS_X  + VOL_BTN_W))) {
                /* 音量：按一下調一格。
                 *
                 * **不能用 wait_release()。** 它會擋住整個播放迴圈 ——
                 * 按著的時候畫面不解碼、音訊也沒人補，實測就是
                 * 「點音量畫面和聲音會停一下、按住會停住」。
                 *
                 * 改成不阻塞的閂鎖：按下就調一格，然後要**連續**幾次讀不到
                 * 觸控才解鎖（一次沒讀到不算 —— 電容觸控本來就會漏報，
                 * board-notes 14.7）。手指按著時 vol_lock 不會遞減，
                 * 所以按住不會連跳；而迴圈全程照常跑。
                 *
                 * 第一版用 nap(180) 當去抖，實測「一點跳 20」：那比一次
                 * 正常點擊還短（18.4 量過約 220ms），手指還在上面就又
                 * 進了一次判定。 */
                if (vol_lock == 0u) {
                    if (x < VOL_MINUS_X + VOL_BTN_W) {
                        g_audio_vol = (g_audio_vol >= VOL_STEP)
                                    ? (g_audio_vol - VOL_STEP) : 0u;
                    } else {
                        g_audio_vol = (g_audio_vol + VOL_STEP > 100u)
                                    ? 100u : (g_audio_vol + VOL_STEP);
                    }
                    audio_set_volume(g_audio_vol);
                    vol_lock = 3u;
                }
                ov_until = HAL_GetTick() + OV_MS;   /* 按著就讓控制列別消失 */
            } else if (y >= PB_Y - PB_HIT && y < PB_Y + PB_H + PB_HIT) {
                /* 拖曳 seek。位移表讓任何一格都能直接定址，成本跟播下一格
                 * 一模一樣 —— 這是打包成單一檔案順帶換到的好處。 */
                int px = x - PB_X;

                if (px < 0) { px = 0; }
                if (px > PB_W) { px = PB_W; }
                idx = (uint32_t)((uint64_t)px * v->count / PB_W);
                if (idx >= v->count) { idx = v->count - 1u; }
                /* 聲音也要跳。只跳畫面的話拖完就永遠對不上了。 */
                (void)audio_wav_seek_ms((uint32_t)((uint64_t)idx * 100000u /
                                                   (v->fps_x100 ? v->fps_x100 : 2400u)));
                ov_until = HAL_GetTick() + OV_MS;
                next_ms  = HAL_GetTick();     /* 跳完重新對時 */
                acc      = 0;
            } else {
                paused   = !paused;
                audio_wav_pause(paused);
                ov_until = HAL_GetTick() + OV_MS;
                wait_release();
            }
        }

        if (paused) {
            /* 暫停時仍要重畫，否則疊加層倒數結束後畫面不會更新。 */
            uint8_t *back = (uint8_t *)(g_front ? FB0_ADDR : FB1_ADDR);

            if (video_decode(idx, back)) {
                if (HAL_GetTick() < ov_until) {
                    draw_overlay(v, idx, true);
                }
                present();
            }
            nap(60);
            next_ms = HAL_GetTick();
            continue;
        }

        /* 按素材的節奏放。小數毫秒累積到 1ms 就補一格，否則 24fps 的
         * 41.67ms 無論取 41 還是 42 都會慢慢漂掉。 */
        next_ms += frame_ms;
        acc     += frame_frac;
        if (acc >= 1000u) {
            acc -= 1000u;
            next_ms += 1u;
        }
        if ((int32_t)(next_ms - HAL_GetTick()) > 0) {
            /* **等待期間一定要繼續補音訊。** 這個忙等一等就是幾十毫秒，
             * 而緩衝只有 170ms —— 不補的話等個幾格就欠載了。 */
            BBOX[149] = 4u;           /* 進入節奏忙等 */
            while ((int32_t)(next_ms - HAL_GetTick()) > 0) {
                audio_wav_pump();
            }
            BBOX[149] = 5u;           /* 離開節奏忙等 */
        } else {
            next_ms = HAL_GetTick();          /* 跟不上就重新對時 */
        }
    }

    /* 記住離開時的位置。**所有出口都會經過這裡** —— 按返回、卡片被拔掉、
     * 都走同一條路出來，不必在每個 break 前面各記一次（那種寫法漏掉一個
     * 就是「有時候記得有時候忘記」，board-notes 14.5 的家族）。 */
    if (vi < MAX_VIDEOS) {
        g_vid_resume[vi] = idx;
        BBOX[147] = idx;            /* 離開時記住第幾格 */
    }

    BBOX[137] = HAL_GetTick();       /* 離開播放的時刻 */
    g_vid_has_audio = false;
    audio_wav_stop();

    /* **一定要走這裡。** DMA 解碼整合進相簿正式路徑曾經把週邊弄壞
     * （board-notes 16.6），video_close() 會做完整拆除並把解碼器還給照片。 */
    video_close();

    /* 播放期間關掉的 D-Cache 在這裡還回來（所有出口都經過這裡）。
     * EnableDCache 會先整體失效再開啟，不會撿到停用期間的舊快取行。 */
    if (dcache_off) {
        SCB_EnableDCache();
    }
}

/* g_dbg_autovideo 要播哪一部：1 = 第一部；2 = **第一部有音軌的**。
 *
 * 抽成函式是因為 g_dbg_autovideo 有**兩個**檢查點（主迴圈與選單），
 * 而主迴圈那個先跑 —— 只改一處完全沒有效果，症狀卻是「音訊整合失敗」。
 * 這跟 18.3 那條「畫面上寫著功能、程式裡沒有做」是同一個家族：
 * 同一件事散在兩個地方，改了一個就以為改完了。 */
static uint32_t autovideo_index(void)
{
    char     wav[PATH_MAX];
    uint32_t k;

    if (g_dbg_autovideo == 2u) {
        for (k = 0; k < g_vid_count; k++) {
            if (wav_path_of(g_vids[k].path, wav, sizeof(wav))) { return k; }
        }
    }
    return 0u;
}

/* 影片清單。沒有影片時不會走到這裡（選單上的按鈕會是暗的）。 */
#define VROW_Y0        140
#define VROW_H         64
/* 可見列數要夾制。原本是有幾部就畫幾部，第 10 部開始會壓到下方的「返回」鈕、
 * 第 11 部以後直接畫到畫面外 —— 而且沒有捲動，等於永遠選不到。 */
#define VROWS_VISIBLE  8
#define VBTN_BACK_Y    720
#define VBTN_BACK_H    62

static uint32_t g_vscroll;

static uint32_t max_vscroll(void)
{
    return (g_vid_count > VROWS_VISIBLE) ? (g_vid_count - VROWS_VISIBLE) : 0u;
}

static void draw_video_list(void)
{
    gfx_set_orientation(false);
    gfx_clear(COL_BG);
    gfx_text_center(GFX_W / 2, 60, "選擇影片", COL_TEXT);

    {
        uint32_t shown = g_vid_count - g_vscroll;

        if (shown > VROWS_VISIBLE) {
            shown = VROWS_VISIBLE;
        }
        for (uint32_t i = 0; i < shown; i++) {
            uint32_t idx = g_vscroll + i;
            int      y   = VROW_Y0 + (int)i * VROW_H;
            uint32_t fps100 = g_vids[idx].fps_x100 ? g_vids[idx].fps_x100
                                                   : 2400u;

            gfx_pill(20, y, GFX_W - 40, VROW_H - 10, COL_PANEL);
            draw_name_clipped(38, y + 18, g_vids[idx].name,
                              GFX_W - 150, COL_TEXT);
            draw_time(GFX_W - 100, y + 18,
                      (uint32_t)((uint64_t)g_vids[idx].count * 100u / fps100),
                      COL_DIM);
        }
        if (g_vid_count > VROWS_VISIBLE) {
            gfx_text_center(GFX_W / 2, VROW_Y0 + VROWS_VISIBLE * VROW_H + 4,
                            "上下滑動看更多", COL_DIM);
        }
    }
    gfx_pill(16, VBTN_BACK_Y, GFX_W - 32, VBTN_BACK_H, COL_PANEL);
    gfx_text_center(GFX_W / 2, VBTN_BACK_Y + 12, "返回", COL_TEXT);
}

static void video_list_screen(void)
{
    bool dirty = true;

    for (;;) {
        int x, y;

        if (dirty) {
            for (int i = 0; i < 2; i++) {     /* 兩塊都要畫 */
                draw_video_list();
                present();
            }
            dirty = false;
        }
        watchdog_feed();
        if (!sd_present()) {
            return;
        }
        if (!screen_poll()) {
            nap(30);
            continue;
        }
        if (!read_touch(&x, &y)) {
            nap(15);
            continue;
        }
        (void)x;
        if (y >= VBTN_BACK_Y && y < VBTN_BACK_Y + VBTN_BACK_H) {
            wait_release();
            return;
        }
        if (y >= VROW_Y0 && y < VROW_Y0 + VROWS_VISIBLE * VROW_H) {
            /* 跟資料夾清單同一套：上下拖曳捲動，單純點一下才是選片。 */
            uint32_t seen = HAL_GetTick();
            int      dy = 0, tx, ty;

            for (;;) {
                watchdog_feed();
                if (read_touch(&tx, &ty)) {
                    int d = ty - y;

                    if (((d < 0) ? -d : d) > ((dy < 0) ? -dy : dy)) {
                        dy = d;
                    }
                    seen = HAL_GetTick();
                } else if ((HAL_GetTick() - seen) > SWIPE_GAP_MS) {
                    break;
                }
                nap(10);
            }

            if (dy <= -SCROLL_MIN_DY || dy >= SCROLL_MIN_DY) {
                int32_t rows = (int32_t)(-dy / VROW_H);
                int32_t ns;

                if (rows == 0) {
                    rows = (dy < 0) ? 1 : -1;
                }
                ns = (int32_t)g_vscroll + rows;
                if (ns < 0) { ns = 0; }
                if (ns > (int32_t)max_vscroll()) { ns = (int32_t)max_vscroll(); }
                if ((uint32_t)ns != g_vscroll) {
                    g_vscroll = (uint32_t)ns;
                    dirty = true;
                }
                continue;
            }
            {
                uint32_t i = g_vscroll + (uint32_t)(y - VROW_Y0) / VROW_H;

                if (i < g_vid_count) {
                    play_video(&g_vids[i]);
                    dirty = true;
                    continue;
                }
            }
            continue;
        }
        wait_release();
    }
}

/* ------------------------------------------------------------------ */
/* 選擇畫面                                                            */
/* ------------------------------------------------------------------ */

#define ROW_Y0       120
#define ROW_H        58
/* 可見列數從 7 減成 6，騰出的 58px 給「方向」那一列。資料夾清單本來就
 * 可以上下滑，少一列的代價比擠不下設定小。 */
#define ROWS_VISIBLE 5
#define BTN_ALL_Y    440
#define BTN_ALL_H    48
#define OR_Y         506
#define OR_H         48
#define OR_X0        110
#define OR_W         100
#define OR_GAP       10
#define SEQ_Y        574
#define SEQ_H        48
#define IV_Y         642
#define IV_H         48
#define IV_X0        110
#define IV_W         74
#define IV_GAP       10
#define BTN_GO_Y     708
#define BTN_GO_H     62
/* 底下那一列是 1~3 顆按鈕，等寬平分 16..GFX_W-16：
 *
 *   0 = 開始播放（一定有）
 *   1 = 最愛（收藏數 > 0 才出現）
 *   2 = 影片（卡上有影格包才出現）
 *
 * 原本是寫死的兩組座標（BTN_GO_W / BTN_VID_X），加第三顆之後組合變成四種，
 * 改成算的比較不會漏掉某一種。 */
#define BOT_GAP      12

static int bottom_slots(void)
{
    return 1 + ((fav_count() > 0u) ? 1 : 0) + (g_vid_count ? 1 : 0);
}

static int bottom_fav_idx(void)
{
    return (fav_count() > 0u) ? 1 : -1;
}

static int bottom_vid_idx(void)
{
    if (!g_vid_count)         { return -1; }
    return (fav_count() > 0u) ? 2 : 1;
}

static void bottom_rect(int idx, int *x, int *w)
{
    int n = bottom_slots();

    *w = ((int)GFX_W - 32 - BOT_GAP * (n - 1)) / n;
    *x = 16 + idx * (*w + BOT_GAP);
}

static const char *const ORIENT_NAME[PHOTO_ORIENT_COUNT] = {
    "直立", "橫向", "自動"
};

static uint32_t g_scroll;

static uint32_t max_scroll(void)
{
    return (g_top_count > ROWS_VISIBLE) ? (g_top_count - ROWS_VISIBLE) : 0u;
}

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

    /* 選單、掃描進度、錯誤訊息都是照直立版面寫死的座標。播放中可能切到
     * 橫向，回到這些畫面之前一定要扳回來，否則版面整個跑掉。 */
    gfx_set_orientation(false);
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

    gfx_text(20, OR_Y + 14, "方向", COL_DIM);
    for (uint32_t o = 0; o < PHOTO_ORIENT_COUNT; o++) {
        int  x  = OR_X0 + (int)o * (OR_W + OR_GAP);
        bool on = ((uint32_t)photo_get_orientation() == o);
        gfx_pill(x, OR_Y, OR_W, OR_H, on ? COL_ACCENT : COL_PANEL);
        gfx_text_center(x + OR_W / 2, OR_Y + 14, ORIENT_NAME[o],
                        on ? COL_BG : COL_TEXT);
    }

    gfx_text(20, SEQ_Y + 14, "順序", COL_DIM);
    for (uint32_t k = 0; k < 2u; k++) {
        int  x  = OR_X0 + (int)k * (OR_W + OR_GAP);
        bool on = (g_shuffle == (k == 0u));

        gfx_pill(x, SEQ_Y, OR_W, SEQ_H, on ? COL_ACCENT : COL_PANEL);
        gfx_text_center(x + OR_W / 2, SEQ_Y + 14, SEQ_NAME[k],
                        on ? COL_BG : COL_TEXT);
    }

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
        int      x, w;

        bottom_rect(0, &x, &w);
        gfx_pill(x, BTN_GO_Y, w, BTN_GO_H, n ? COL_ACCENT : COL_PANEL);
        if (n) {
            gfx_text_center(x + w / 2, BTN_GO_Y + 12, "開始播放", COL_BG);
            if (bottom_slots() == 1) {      /* 只有一顆時才擠得下張數 */
                gfx_number_right(GFX_W - 40, BTN_GO_Y + 12, n, COL_BG);
            }
        } else {
            gfx_text_center(x + w / 2, BTN_GO_Y + 20,
                            (bottom_slots() > 1) ? "無照片"
                                                 : "沒有選取任何資料夾",
                            COL_DIM);
        }

        /* 收藏數是 0 就不畫這顆 —— 沒有內容的入口只會讓人按了失望。 */
        if (bottom_fav_idx() >= 0) {
            bottom_rect(bottom_fav_idx(), &x, &w);
            gfx_pill(x, BTN_GO_Y, w, BTN_GO_H, COL_PANEL);
            gfx_text_center(x + w / 2, BTN_GO_Y + 12, "最愛", COL_HEART);
        }
        if (bottom_vid_idx() >= 0) {
            bottom_rect(bottom_vid_idx(), &x, &w);
            gfx_pill(x, BTN_GO_Y, w, BTN_GO_H, COL_PANEL);
            gfx_text_center(x + w / 2, BTN_GO_Y + 12, "影片", COL_ACCENT);
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

    BBOX[12]++;

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
        /* 除錯旗標也要在這裡看：選單這個迴圈在等觸控時不會回到主迴圈，
         * 只在主迴圈檢查的話，SWD 設旗標永遠不會生效。 */
        if (g_dbg_favtest || g_dbg_wrtest || g_dbg_cardreinit ||
            g_dbg_audiotest || (BBOX[16] >> 16) == 0x5A5Bu) {
            return false;               /* 交回主迴圈去跑測試 */
        }
        if (g_dbg_autovideo && g_vid_count > 0u) {
            BBOX[127] = autovideo_index();
            /* **吃掉旗標**，跟其他除錯旗標一樣一次性。
             * 不吃的話按「返回」離開播放之後，這裡立刻又把同一部從頭播 ——
             * 使用者看到的是「返回鍵回不去選單，只會回到影片開頭」，
             * 而返回鍵其實是好的。除錯用的觸發不該改變正式路徑的行為
             * （board-notes 16.3）。 */
            g_dbg_autovideo = 0u;
            play_video(&g_vids[BBOX[127]]);
            dirty = true;
            continue;
        }
        /* 除錯：直接開始放照片，讓遠端也追得動暫停/繼續的流程。 */
        if (g_dbg_autoplay && selected_photo_count() > 0u) {
            build_order();
            return true;
        }
        if (!screen_poll()) {
            /* 螢幕關著時不吃觸控，否則會在看不見的情況下改到設定。 */
            nap(30);
            continue;
        }

        if (!read_touch(&x, &y)) {
            nap(15);
            continue;
        }

        if (y >= ROW_Y0 && y < ROW_Y0 + ROWS_VISIBLE * ROW_H) {
            /* 上下拖曳捲動、單純點一下才是勾選。
             *
             * 這個捲動先前**根本沒做** —— g_scroll 只有被讀、從來沒被寫過，
             * 所以第六個以後的資料夾看不到也選不到，而畫面上還寫著
             * 「上下滑動看更多」。用位移量分辨拖曳與點選，兩者不會互搶。 */
            uint32_t seen = HAL_GetTick();
            int      dy = 0, tx, ty;

            for (;;) {
                watchdog_feed();
                if (read_touch(&tx, &ty)) {
                    int d = ty - y;

                    if (((d < 0) ? -d : d) > ((dy < 0) ? -dy : dy)) {
                        dy = d;
                    }
                    seen = HAL_GetTick();
                } else if ((HAL_GetTick() - seen) > SWIPE_GAP_MS) {
                    break;
                }
                nap(10);
            }

            if (dy <= -SCROLL_MIN_DY || dy >= SCROLL_MIN_DY) {
                /* 往上拉（dy < 0）= 往清單後面看。不足一列也要動一列，
                 * 否則短拉會完全沒反應，使用者會以為壞掉。 */
                int32_t rows = (int32_t)(-dy / ROW_H);
                int32_t ns;

                if (rows == 0) {
                    rows = (dy < 0) ? 1 : -1;
                }
                ns = (int32_t)g_scroll + rows;
                if (ns < 0) { ns = 0; }
                if (ns > (int32_t)max_scroll()) { ns = (int32_t)max_scroll(); }
                if ((uint32_t)ns != g_scroll) {
                    g_scroll = (uint32_t)ns;
                    dirty = true;
                }
            } else {
                uint32_t i = g_scroll + (uint32_t)(y - ROW_Y0) / ROW_H;

                if (i < g_top_count) {
                    g_top[i].selected = !g_top[i].selected;
                    dirty = true;
                }
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
        } else if (y >= OR_Y && y < OR_Y + OR_H) {
            for (uint32_t o = 0; o < PHOTO_ORIENT_COUNT; o++) {
                int bx = OR_X0 + (int)o * (OR_W + OR_GAP);
                if (x >= bx && x < bx + OR_W) {
                    photo_set_orientation((photo_orient_t)o);
                    dirty = true;
                }
            }
        } else if (y >= SEQ_Y && y < SEQ_Y + SEQ_H) {
            for (uint32_t k = 0; k < 2u; k++) {
                int bx = OR_X0 + (int)k * (OR_W + OR_GAP);

                if (x >= bx && x < bx + OR_W) {
                    g_shuffle = (k == 0u);
                    dirty = true;
                }
            }
        } else if (y >= IV_Y && y < IV_Y + IV_H) {
            for (uint32_t s = 2; s <= 5; s++) {
                int bx = IV_X0 + (int)(s - 2) * (IV_W + IV_GAP);
                if (x >= bx && x < bx + IV_W) {
                    g_interval_s = s;
                    dirty = true;
                }
            }
        } else if (y >= BTN_GO_Y && y < BTN_GO_Y + BTN_GO_H) {
            int bx, bw;

            if (bottom_vid_idx() >= 0) {
                bottom_rect(bottom_vid_idx(), &bx, &bw);
                if (x >= bx && x < bx + bw) {
                    wait_release();
                    video_list_screen();
                    dirty = true;
                    continue;
                }
            }
            if (bottom_fav_idx() >= 0) {
                bottom_rect(bottom_fav_idx(), &bx, &bw);
                if (x >= bx && x < bx + bw) {
                    g_fav_mode = true;
                    build_order();
                    if (g_order_count > 0u) {
                        wait_release();
                        return true;
                    }
                    /* 清單裡的照片一張都對不上：換過卡，或檔案被搬走了。 */
                    g_fav_mode = false;
                    show_message("最愛的照片都不在卡上",
                                 "可能換過卡或移動過檔案");
                    nap(2000);
                    dirty = true;
                    wait_release();
                    continue;
                }
            }
            bottom_rect(0, &bx, &bw);
            if (x >= bx && x < bx + bw && selected_photo_count() > 0U) {
                g_fav_mode = false;
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
    gfx_set_orientation(false);
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
    /* 腳印切到這裡面：這是開機腳印 5 之後唯一會做事的呼叫，而它做的三件事
     * （清畫面 = 寫 750KB PSRAM、畫字、等垂直消隱）風險完全不同。 */
    BBOX[26] = 10u;
    gfx_set_orientation(false);
    for (int i = 0; i < 2; i++) {
        BBOX[26] = 11u + (uint32_t)i * 10u;
        gfx_clear(COL_BG);
        BBOX[26] = 12u + (uint32_t)i * 10u;
        gfx_text_center(GFX_W / 2, GFX_H / 2 - 40, line1, COL_TEXT);
        if (line2) {
            gfx_text_center(GFX_W / 2, GFX_H / 2 + 10, line2, COL_DIM);
        }
        BBOX[26] = 13u + (uint32_t)i * 10u;
        present();
    }
    BBOX[26] = 30u;
}

/* 螢幕關著時停在這裡。回傳 false 代表卡片被拔掉。 */
static bool wait_screen_on(void)
{
    while (!screen_poll()) {
        watchdog_feed();
        if (!sd_present()) {
            return false;
        }
        nap(100);
    }
    return true;
}

/* 判斷觸控是短按還是長按。回傳 0=沒碰到，1=短按，2=長按。
 *
 * 短按暫停／繼續、長按回選單。原本短按就直接離開，結果想停下來看某一張時
 * 只能被踢回選單，反而看不到。 */


/* 暫停 / 繼續的視覺回饋。
 *
 * 按鍵操作沒有觸覺以外的回饋，使用者按下去看畫面沒變會不確定有沒有成功。
 * 在畫面中央閃一個圖示一秒再還原 —— 蓋住的區域先備份，還原後照片完整。 */
#define ICON_SZ     140
#define ICON_SAVE   ((uint16_t *)0x91F00000u)   /* 140x140x2 = 39KB */

/* 圖示置中的位置。畫布尺寸隨方向改變，所以不能是常數。 */
static int icon_x(void) { return (gfx_width()  - ICON_SZ) / 2; }
static int icon_y(void) { return (gfx_height() - ICON_SZ) / 2; }

static void icon_backup(bool restore)
{
    uint16_t *front = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);
    int       ix = icon_x(), iy = icon_y();

    /* 兩種方向下「哪個軸在實體記憶體裡連續」剛好相反：
     *   直立：邏輯 x 是實體列，所以沿著邏輯 y 走才連續
     *   橫向：畫布與面板同向，沿著邏輯 x 走就連續
     * 兩者都是每次搬 ICON_SZ 個連續像素，只差基底怎麼算。 */
    for (uint32_t i = 0; i < ICON_SZ; i++) {
        uint16_t *fb;
        uint16_t *sv = ICON_SAVE + i * ICON_SZ;

        if (gfx_is_landscape()) {
            fb = front + (uint32_t)(iy + (int)i) * PHYS_W + (uint32_t)ix;
        } else {
            fb = front + (uint32_t)(gfx_width() - 1 - (ix + (int)i)) * PHYS_W
                       + (uint32_t)iy;
        }
        if (restore) {
            memcpy(fb, sv, ICON_SZ * sizeof(uint16_t));
        } else {
            memcpy(sv, fb, ICON_SZ * sizeof(uint16_t));
        }
    }
}

static void flash_icon(bool paused)
{
    uint16_t *back  = gfx_framebuffer();
    uint16_t *front = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);
    int cx = gfx_width()  / 2;
    int cy = gfx_height() / 2;
    int ICON_X = icon_x(), ICON_Y = icon_y();

    icon_backup(false);
    gfx_set_framebuffer(front);

    /* 半透明底：把區域壓暗，圖示才浮得出來。 */
    for (int j = 0; j < ICON_SZ; j++) {
        for (int i = 0; i < ICON_SZ; i++) {
            uint16_t v = gfx_get_pixel(ICON_X + i, ICON_Y + j);
            gfx_pixel(ICON_X + i, ICON_Y + j,
                      (uint16_t)((((v >> 11) & 0x1Fu) >> 1) << 11 |
                                 (((v >> 5) & 0x3Fu) >> 1) << 5 |
                                 ((v & 0x1Fu) >> 1)));
        }
    }

    if (paused) {
        gfx_fill_rect(cx - 30, cy - 36, 20, 72, COL_TEXT);   /* ▮▮ */
        gfx_fill_rect(cx + 10, cy - 36, 20, 72, COL_TEXT);
    } else {
        /* ▶：逐列畫，寬度隨高度線性收斂 */
        for (int j = 0; j < 72; j++) {
            int half = (j < 36) ? j : (71 - j);
            gfx_fill_rect(cx - 24, cy - 36 + j, 12 + half, 1, COL_TEXT);
        }
    }

    gfx_set_framebuffer(back);
    nap(400);
    icon_backup(true);
}

/* 切換方向時閃一下目前的模式。
 *
 * 三段循環沒有回饋的話使用者不知道自己切到哪了 —— 沿用暫停圖示那套
 * 「壓暗背景、蓋上內容、一秒後還原」的機制。 */
static void flash_orient(void)
{
    uint16_t *back  = gfx_framebuffer();
    uint16_t *front = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);
    int cx = gfx_width() / 2;
    int cy = gfx_height() / 2;
    int ix = icon_x(), iy = icon_y();

    icon_backup(false);
    gfx_set_framebuffer(front);

    for (int j = 0; j < ICON_SZ; j++) {
        for (int i = 0; i < ICON_SZ; i++) {
            uint16_t v = gfx_get_pixel(ix + i, iy + j);
            gfx_pixel(ix + i, iy + j,
                      (uint16_t)((((v >> 11) & 0x1Fu) >> 1) << 11 |
                                 (((v >> 5) & 0x3Fu) >> 1) << 5 |
                                 ((v & 0x1Fu) >> 1)));
        }
    }
    gfx_text_center(cx, cy - 10, ORIENT_NAME[photo_get_orientation()],
                    COL_TEXT);

    gfx_set_framebuffer(back);
    nap(400);
    icon_backup(true);
}

/* 暫停中的滑動判定。
 * 邏輯座標：x 是橫向（0~479），y 是縱向（0~799，往下遞增）。
 * 縱向門檻設得比橫向大，因為畫面高是寬的 1.67 倍，手滑起來也比較長。 */
#define SWIPE_MIN_X  60
#define SWIPE_MIN_Y  120

/* 暫停：畫面停在目前這張。按鍵控制暫停/選單，滑動翻頁。 */
#define PAUSE_RESUME 0
#define PAUSE_MENU   1
#define PAUSE_PREV   2
#define PAUSE_NEXT   3
#define PAUSE_ROTATE 4
#define PAUSE_FAV    6           /* 愛心：加入／移出最愛（5 是拉桿）*/

/* ---- 暫停中的疊加層（覆刻影片播放器）----------------------------
 *
 * 版面與互動跟 play_video() 那套一致，兩邊看起來才是同一個介面：
 *
 *   上排   返回（左）   方向（中）   暫停（右，指示用）
 *   下方   拉桿：目前第幾張 / 共幾張，可以拖曳翻頁
 *
 * 點畫面暫停、再點一下（疊加層以外的地方）繼續。疊加層停留 OV_MS 之後
 * 自己收起來但**維持暫停** —— 收起只是讓照片看得完整。收起狀態下點一下
 * 只把它叫回來，不會直接觸發動作，否則使用者看不到按鈕在哪就先按到東西。
 *
 * 疊加層直接畫在「顯示中」的那塊 buffer 上，所以先把底下的畫面存起來、
 * 離開時原封還原 —— 重新解一張照片要 1.5 秒，存還原只要幾毫秒。
 *
 * **一律用直立座標**，不跟著照片的方向轉：AUTO 會逐張決定畫布方向，
 * 但使用者的手一直在同一個地方，會轉的應該只有照片。而且 read_touch()
 * 回傳的永遠是直立座標，統一成直立兩邊才對得上。 */

/* 借用照片路徑的 YCbCr 區。暫停中不解碼，而按下任何按鈕之後都會**先還原
 * 畫面才去解碼**，兩者不會互相踩到。 */
#define CTL_SAVE     ((uint16_t *)0x90600000u)

/* 上排四顆：返回 / 方向 / 愛心 / 暫停。
 *
 * 原本是三顆 120px 剛好排滿 480。加愛心之後改成 4x100 + 5x16 的間距，
 * 也是剛好 480 —— 不必動 OV_TOP_H，存還原的橫帶範圍完全不變。 */
#define OV_TOP_Y     36
#define OV_TOP_H     56
#define OV_PILL_W    100
#define OV_GAP       16
#define OV_BACK_X    OV_GAP                                  /* 16  */
#define OV_ORIENT_X  (OV_BACK_X   + OV_PILL_W + OV_GAP)      /* 132 */
#define OV_FAV_X     (OV_ORIENT_X + OV_PILL_W + OV_GAP)      /* 248 */
#define OV_PAUSE_X   (OV_FAV_X    + OV_PILL_W + OV_GAP)      /* 364 */

/* 拉桿與翻書手勢是**互補**的，不是二選一：
 *
 *   拉桿（下方那一帶）  粗調 —— 一下跳到大概的位置
 *   整片畫面左右拉      微調 —— 前一張／後一張
 *
 * 幾何沿用影片疊加層的常數，兩邊看起來才是同一套介面。 */
#define PB_X         40
#define PB_W         400
#define PB_Y         700
#define PB_H         14
#define PB_HIT       60
#define OV_BOT_Y     (PB_Y - 16)
#define OV_BOT_H     96

#define PAUSE_SEEK   5           /* 拉桿區域，拖曳定位 */
#define SEEK_MIN_DX  50          /* 水平拉多少才算翻頁 */

static uint32_t g_ov_index;      /* 疊加層要顯示的張數（由 pause_session 給） */
static uint32_t g_seek_target;   /* 拖曳拉桿結束時停在哪一張 */

/* 愛心的狀態，進 paused_loop() 之前由 pause_session 算好（純記憶體查表）。
 *
 * LOCKED 是清單檔載入失敗（讀不到也建不起來）的情況 —— 收藏會存不下來，
 * 所以那顆按鈕不接受點擊，免得使用者以為收藏成功了。 */
typedef enum {
    FAVBTN_OFF = 0,              /* 未收藏 */
    FAVBTN_ON,                   /* 已收藏 */
    FAVBTN_LOCKED,               /* 清單檔不可用 */
} favbtn_t;

static favbtn_t g_ov_fav;

/* 剛恢復播放的時刻。恢復之後短時間內不理會觸控 —— 否則同一次觸碰
 * （或放開瞬間的彈跳）會立刻又被播放迴圈判成「暫停」，症狀是
 * 「點一下播放不了」。實體鍵不會有這個問題，因為它是閂鎖旗標，
 * 不會被兩個迴圈各吃一次；使用者回報「只有實體鍵按得動」正是這個差別。 */
static uint32_t g_resume_ms;
#define RESUME_GUARD_MS  700u

/* 存還原一條橫帶（邏輯 y0 起算 h 列，整個畫布寬）。
 * 直立時邏輯 x 是實體列，所以沿著邏輯 y 走才連續。 */
static uint16_t *strip_backup(int y0, int h, uint16_t *sv, bool restore)
{
    uint16_t *front = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);

    for (int i = 0; i < (int)GFX_W; i++) {
        uint16_t *fb = front + (uint32_t)((int)GFX_W - 1 - i) * PHYS_W
                             + (uint32_t)y0;

        if (restore) { memcpy(fb, sv, (size_t)h * 2u); }
        else         { memcpy(sv, fb, (size_t)h * 2u); }
        sv += h;
    }
    return sv;
}

static void ctl_backup(bool restore)
{
    uint16_t *sv = CTL_SAVE;

    sv = strip_backup(OV_TOP_Y, OV_TOP_H, sv, restore);
    (void)strip_backup(OV_BOT_Y, OV_BOT_H, sv, restore);
}

/* 只重畫下方的拉桿。拖曳時每十毫秒就要更新一次，不能整個疊加層重畫。 */
static void ov_draw_bar(uint32_t idx)
{
    uint16_t *back   = gfx_framebuffer();
    uint16_t *front  = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);
    bool      was_ls = gfx_is_landscape();
    uint32_t  total  = g_order_count ? g_order_count : 1u;
    uint32_t  done_w = (uint32_t)PB_W * idx / total;

    gfx_set_orientation(false);
    gfx_set_framebuffer(front);

    gfx_fill_rect(0, OV_BOT_Y, (int)GFX_W, OV_BOT_H, COL_BG);
    gfx_fill_rect(PB_X, PB_Y, PB_W, PB_H, COL_LINE);
    if (done_w != 0u) {
        gfx_fill_rect(PB_X, PB_Y, (int)done_w, PB_H, COL_ACCENT);
    }
    gfx_fill_rect(PB_X + (int)done_w - 3, PB_Y - 8, 6, PB_H + 16, COL_TEXT);

    gfx_number(PB_X, PB_Y + 30, idx + 1u, COL_TEXT);
    gfx_number_right(PB_X + PB_W, PB_Y + 30, total, COL_DIM);

    gfx_set_framebuffer(back);
    gfx_set_orientation(was_ls);
}

/* 愛心。字型裡沒有這個符號，而且用畫的才能同時表達實心／空心／不可按，
 * 所以直接用原語組：兩個圓當上緣，一個倒三角當下緣。
 *
 * 空心是「先畫大的、再用底色畫小一號的」挖出來的，不必另外寫外框演算法。 */
static void draw_heart(int cx, int cy, int r, uint16_t color)
{
    int i, span = r + r / 3;

    gfx_disc(cx - r / 2, cy - r / 3, r / 2, color);
    gfx_disc(cx + r / 2, cy - r / 3, r / 2, color);
    for (i = 0; i < span; i++) {
        int half = r - (i * r) / span;

        if (half > 0) {
            gfx_fill_rect(cx - half, cy - r / 3 + i, half * 2, 1, color);
        }
    }
}

/* 畫愛心那一格。ctl_draw() 與收藏狀態改變之後都會呼叫。
 *
 * 只重畫這一顆藥丸，範圍完全落在 ctl_backup() 存過的上排橫帶裡，
 * 所以 ov_hide() 照樣還原得乾淨 —— 不會多出一條沒被備份的區域。 */
static void ctl_draw_fav(void)
{
    uint16_t fg = (g_ov_fav == FAVBTN_LOCKED) ? COL_LINE : COL_HEART;
    int      cx = OV_FAV_X + OV_PILL_W / 2;
    int      cy = OV_TOP_Y + OV_TOP_H / 2;

    gfx_pill(OV_FAV_X, OV_TOP_Y, OV_PILL_W, OV_TOP_H, COL_PANEL);
    draw_heart(cx, cy, 15, fg);
    if (g_ov_fav != FAVBTN_ON) {
        /* 挖空：小一號的底色愛心蓋上去就剩下外框。 */
        draw_heart(cx, cy, 10, COL_PANEL);
    }
}

static void ctl_draw(void)
{
    uint16_t *back   = gfx_framebuffer();
    uint16_t *front  = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);
    bool      was_ls = gfx_is_landscape();

    gfx_set_orientation(false);
    gfx_set_framebuffer(front);

    gfx_pill(OV_BACK_X, OV_TOP_Y, OV_PILL_W, OV_TOP_H, COL_PANEL);
    gfx_text_center(OV_BACK_X + OV_PILL_W / 2, OV_TOP_Y + 16, "返回", COL_TEXT);

    ctl_draw_fav();

    /* 方向那顆直接顯示目前模式（自動／直立／橫向），按下去之前就知道狀態。 */
    gfx_pill(OV_ORIENT_X, OV_TOP_Y, OV_PILL_W, OV_TOP_H, COL_PANEL);
    gfx_text_center(OV_ORIENT_X + OV_PILL_W / 2, OV_TOP_Y + 16,
                    ORIENT_NAME[photo_get_orientation()], COL_TEXT);

    gfx_pill(OV_PAUSE_X, OV_TOP_Y, OV_PILL_W, OV_TOP_H, COL_PANEL);
    gfx_text_center(OV_PAUSE_X + OV_PILL_W / 2, OV_TOP_Y + 16,
                    "暫停", COL_ACCENT);

    gfx_set_framebuffer(back);
    gfx_set_orientation(was_ls);

    ov_draw_bar(g_ov_index);
}

/* 收藏狀態改變之後只重畫愛心那一顆，不整個疊加層重畫（跟 ov_draw_bar
 * 同一個模式）。範圍在已備份的上排橫帶內，還原不受影響。 */
static void ov_refresh_fav(void)
{
    uint16_t *back   = gfx_framebuffer();
    uint16_t *front  = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);
    bool      was_ls = gfx_is_landscape();

    gfx_set_orientation(false);
    gfx_set_framebuffer(front);
    ctl_draw_fav();
    gfx_set_framebuffer(back);
    gfx_set_orientation(was_ls);
}

/* 在拉桿那條橫帶上顯示一行字（成功／失敗的回饋）。 */
static void ov_draw_note(const char *msg, uint16_t color)
{
    uint16_t *back   = gfx_framebuffer();
    uint16_t *front  = (uint16_t *)(g_front ? FB1_ADDR : FB0_ADDR);
    bool      was_ls = gfx_is_landscape();

    gfx_set_orientation(false);
    gfx_set_framebuffer(front);

    /* 座標必須落在 OV_BOT_Y..+OV_BOT_H（684..780）這條**已備份**的橫帶裡，
     * 否則 ov_hide() 擦不掉，字就永久烙在那塊 buffer 上（board-notes 18.6）。 */
    gfx_fill_rect(0, OV_BOT_Y, (int)GFX_W, OV_BOT_H, COL_BG);
    gfx_text_center((int)GFX_W / 2, PB_Y + 20, msg, color);

    gfx_set_framebuffer(back);
    gfx_set_orientation(was_ls);
}

/* 目前顯示的那張照片的完整路徑。沒有有效照片時回傳 NULL。 */
static const char *cur_photo_path(void)
{
    if (g_order_count == 0u || g_ov_index >= g_order_count) {
        return NULL;
    }
    return PLAYLIST_BASE + g_order[g_ov_index] * PATH_MAX;
}

/* 切換收藏。
 *
 * 清單版只寫一格（一個磁區），幾毫秒就回來，所以不需要進度條 ——
 * 第一版是複製整張照片才需要。失敗訊息仍然畫在 ctl_backup() 存過的
 * 橫帶裡，ov_hide() 才還原得乾淨（board-notes 18.6）。 */
static void fav_toggle_current(void)
{
    const char  *p = cur_photo_path();
    bool         was_on;
    fav_result_t r;

    if (p == NULL || g_ov_fav == FAVBTN_LOCKED) {
        return;
    }
    was_on = (g_ov_fav == FAVBTN_ON);
    r      = was_on ? fav_remove(p) : fav_add(p);

    if (r == FAV_OK) {
        g_ov_fav = was_on ? FAVBTN_OFF : FAVBTN_ON;
    } else if (r == FAV_ERR_FULL) {
        ov_draw_note("最愛已滿（上限 256 張）", COL_HEART);
        nap(1800);
    } else {
        ov_draw_note(was_on ? "移出最愛失敗" : "加入最愛失敗", COL_HEART);
        nap(1500);
    }

    ov_refresh_fav();
    ov_draw_bar(g_ov_index);        /* 把訊息蓋回張數顯示 */
}

/* 回傳動作代碼；-1 = 疊加層以外（＝繼續播放）。
 * 「暫停」那顆只是指示，點它跟點空白處一樣。 */
static int ctl_hit(int x, int y)
{
    if (y >= OV_TOP_Y && y < OV_TOP_Y + OV_TOP_H) {
        if (x >= OV_BACK_X && x < OV_BACK_X + OV_PILL_W) {
            return PAUSE_MENU;
        }
        if (x >= OV_ORIENT_X && x < OV_ORIENT_X + OV_PILL_W) {
            return PAUSE_ROTATE;
        }
        /* 不可按的時候當成沒點到，讓它落到「整片畫面」那條去 —— 使用者
         * 會看到繼續播放而不是一顆沒反應的按鈕。 */
        if (g_ov_fav != FAVBTN_LOCKED &&
            x >= OV_FAV_X && x < OV_FAV_X + OV_PILL_W) {
            return PAUSE_FAV;
        }
    }
    if (y >= PB_Y - PB_HIT && y < PB_Y + PB_H + PB_HIT) {
        return PAUSE_SEEK;
    }
    return -1;
}

/* 疊加層目前有沒有蓋上去。存還原必須成對，所以集中在這兩個函式裡
 * （board-notes 14.5 那個對稱性教訓）。 */
static bool g_ov_shown;

static void ov_show(void)
{
    if (!g_ov_shown) {
        ctl_backup(false);
        ctl_draw();
        g_ov_shown = true;
    }
}

static void ov_hide(void)
{
    if (g_ov_shown) {
        ctl_backup(true);
        g_ov_shown = false;
    }
}

/* 量一次拖曳：等到手指離開，回傳整段過程中的**最大水平位移**。
 *
 * 取最大位移而不是放開瞬間的位置，而且容忍中途漏報 —— 快速滑動時觸控
 * 控制器會掉幾筆座標，一讀不到就結束的話位移只算到中斷點為止
 * （board-notes 14.7）。
 *
 * 順帶：它回來時手指一定已經離開，所以呼叫端不必再 wait_release()。 */
static int measure_drag(int x0)
{
    uint32_t seen = HAL_GetTick();
    int      dx = 0, x, y;

    for (;;) {
        watchdog_feed();
        if (read_touch(&x, &y)) {
            int d = x - x0;

            if (((d < 0) ? -d : d) > ((dx < 0) ? -dx : dx)) {
                dx = d;
            }
            seen = HAL_GetTick();
        } else if ((HAL_GetTick() - seen) > SWIPE_GAP_MS) {
            break;                      /* 真的放開了 */
        }
        nap(10);
    }
    return dx;
}

static int paused_loop_inner(void);

/* 只有「剛進暫停」才閃那顆圖示（目前已經不閃了，保留旗標備用）。 */
static bool g_pause_flash;

/* 除錯：SWD 寫入 PAUSE_* 就當成按了那一顆。觸控沒辦法遠端重現，
 * 沒有這個鉤子就只能靠使用者回報。不寫（維持 -1）等於不存在。 */
volatile int32_t  g_dbg_inject = -1;
volatile uint32_t g_dbg_pause_entries;
volatile int32_t  g_dbg_last_action;
volatile int32_t  g_dbg_tx, g_dbg_ty, g_dbg_hit;

/* 所有出口都會經過這裡，不會有「某條路徑忘了還原」的問題。 */
static int paused_loop(void)
{
    int a;

    g_ov_shown = false;
    ov_show();
    a = paused_loop_inner();
    ov_hide();
    return a;
}


static int paused_loop_inner(void)
{
    g_paused    = 1u;
    g_req_pause = 0u;               /* 消化掉進來的那一次 */
    /* 不再閃暫停圖示。控制列跳出來本身就是最清楚的回饋，而那個圖示要停
     * 幾百毫秒，等於每次進暫停都先卡一下。 */
    g_pause_flash = false;
    /* 把上一個動作殘留的觸碰吃掉。放在**進來的時候**而不是「按下去之後」——
     * 按鈕要按下就有反應，不能等手指離開才動作。 */
    wait_release();

    g_dbg_pause_entries++;
    {
    uint32_t ov_until = HAL_GetTick() + OV_MS;

    for (;;) {
        int x0, y0;

        watchdog_feed();
        /* 疊加層過了時間就自己收起來，但**維持暫停** —— 跟影片一樣，
         * 收起來只是讓照片看得完整，不代表繼續播。 */
        if (g_ov_shown && (int32_t)(HAL_GetTick() - ov_until) >= 0) {
            ov_hide();
        }
        if (g_dbg_inject >= 0) {
            int hit = g_dbg_inject;

            g_dbg_inject = -1;
            /* 收藏要走跟觸控**同一條**路：就地做完、留在暫停。
             * 直接 return 的話會掉到下面翻頁的預設值去（dir 算出 +1），
             * 症狀是「注入 6 卻換了一張照片」。
             *
             * 疊加層可能已經自己收起來了，而進度條畫在它備份過的橫帶上，
             * 所以先確保它蓋著（ov_show 本身是冪等的）。 */
            if (hit == PAUSE_FAV) {
                ov_show();
                fav_toggle_current();
                ov_until          = HAL_GetTick() + OV_MS;
                g_dbg_last_action = hit;
                continue;
            }
            if (hit != PAUSE_ROTATE) {
                g_paused = 0u;
            }
            g_dbg_last_action = hit;
            return hit;
        }
        if (!sd_present()) {
            g_req_pause = 0u;
            g_paused    = 0u;
            return PAUSE_MENU;
        }
        if (g_req_pause) {
            g_req_pause = 0u;
            g_paused    = 0u;
            flash_icon(false);
            return PAUSE_RESUME;
        }

        if (read_touch(&x0, &y0)) {
            int hit;

            /* 疊加層收起來之後，點一下就是繼續播放。
             *
             * 原本是「先把疊加層叫回來」（照影片那套），但暫停中的疊加層
             * 三秒就收起，使用者等一下再點就一定要點兩下才播得起來 ——
             * 回報的「要慢點兩下」正是這個。點一下切換播放/暫停優先。 */
            if (!g_ov_shown) {
                int dx = measure_drag(x0);

                g_paused = 0u;
                if (dx <= -SEEK_MIN_DX) {
                    g_dbg_last_action = PAUSE_PREV;
                    return PAUSE_PREV;
                }
                if (dx >= SEEK_MIN_DX) {
                    g_dbg_last_action = PAUSE_NEXT;
                    return PAUSE_NEXT;
                }
                g_dbg_last_action = PAUSE_RESUME;
                return PAUSE_RESUME;
            }
            ov_until = HAL_GetTick() + OV_MS;   /* 有互動就延長 */
            hit = ctl_hit(x0, y0);

            g_dbg_touch_hit++;
            g_dbg_tx  = x0;
            g_dbg_ty  = y0;
            g_dbg_hit = hit;

            if (hit == PAUSE_SEEK) {
                /* 拖曳定位。手指還在畫面上時只更新把手與張數（很便宜），
                 * 放開才真的去解那一張 —— 每張要 1.5 秒，邊拖邊解會卡死。
                 *
                 * 一個像素約等於十張（4254 張攤在 400px 上），所以這是
                 * 粗調；要精準前後一張請用整片畫面的左右拉。 */
                uint32_t total = g_order_count ? g_order_count : 1u;
                uint32_t tgt   = g_ov_index;
                uint32_t seen  = HAL_GetTick();
                int      lastx = x0, x, y, px;

                for (;;) {
                    watchdog_feed();
                    if (read_touch(&x, &y)) {
                        lastx = x;
                        seen  = HAL_GetTick();
                    } else if ((HAL_GetTick() - seen) > SWIPE_GAP_MS) {
                        break;              /* 真的放開了 */
                    }
                    px = lastx - PB_X;
                    if (px < 0)    { px = 0; }
                    if (px > PB_W) { px = PB_W; }
                    tgt = (uint32_t)(((uint64_t)px * total) / PB_W);
                    if (tgt >= total) { tgt = total - 1u; }
                    if (tgt != g_ov_index) {
                        g_ov_index = tgt;
                        ov_draw_bar(tgt);
                    }
                    nap(10);
                }
                g_seek_target     = tgt;
                g_dbg_last_action = PAUSE_SEEK;
                return PAUSE_SEEK;
            }

            /* 收藏就地處理，不離開這個迴圈 —— 疊加層必須維持蓋著，
             * 進度條才畫得進已備份的橫帶（board-notes 18.6）。
             * 而且使用者當場就看到愛心變實心，不必等重畫。 */
            if (hit == PAUSE_FAV) {
                fav_toggle_current();
                ov_until = HAL_GetTick() + OV_MS;
                /* 手指還在螢幕上的話，下一圈會把同一次觸碰再算一次 ——
                 * 症狀是「按一下卻收藏又取消」（board-notes 18.1 那個
                 * 一次觸碰被吃兩次的家族）。 */
                wait_release();
                continue;
            }

            if (hit >= 0) {
                /* 按下就回報，不等手指離開。切方向要留在暫停，
                 * 讓使用者當場看到結果。 */
                if (hit != PAUSE_ROTATE) {
                    g_paused = 0u;
                }
                g_dbg_last_action = hit;
                return hit;
            }

            /* 控制以外的整片畫面：拖曳＝翻頁（翻書那種感覺），
             * 單純點一下＝繼續播放。
             *
             * measure_drag() 回來時手指一定已經離開，所以「繼續」這條也
             * 順便滿足了「要等手指離開才回報」—— 不然恢復播放之後，
             * 播放迴圈的「點畫面暫停」會把同一次觸碰再吃一次，
             * 症狀就是「點一下播放不了」。 */
            {
                int dx = measure_drag(x0);

                g_paused = 0u;
                if (dx <= -SEEK_MIN_DX) {
                    g_dbg_last_action = PAUSE_PREV;
                    return PAUSE_PREV;
                }
                if (dx >= SEEK_MIN_DX) {
                    g_dbg_last_action = PAUSE_NEXT;
                    return PAUSE_NEXT;
                }
                g_dbg_last_action = PAUSE_RESUME;
                return PAUSE_RESUME;
            }
        }
        nap(8);
    }
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

        watchdog_feed();
        if (!sd_present()) {
            return 2;
        }
        if (!screen_poll()) {
            /* 亮度 0（螢幕關著）：不累計時間，再點亮時這張還有完整時間。 */
            nap(100);
            last = HAL_GetTick();
            continue;
        }

        now = HAL_GetTick();
        waited += now - last;
        last = now;

        if (g_req_pause) {
            return 3;               /* 交給 slideshow 統一處理暫停 */
        }

        /* 播放中點一下畫面就暫停並叫出控制列。
         *
         * 原本只有實體鍵能暫停，而滑動手勢只在暫停中才有作用 —— 等於
         * 「要先知道有那顆按鍵，才進得去操作介面」。點畫面是最直覺的入口，
         * 而且跟影片播放器的操作方式一致。 */
        {
            int tx, ty;

            if ((HAL_GetTick() - g_resume_ms) > RESUME_GUARD_MS &&
                read_touch(&tx, &ty)) {
                (void)tx; (void)ty;
                /* 不在這裡等放開 —— paused_loop_inner 進來就會吃掉殘留的
                 * 觸碰。在這裡等等於讓「點一下叫出控制列」多花兩百毫秒。 */
                return 3;
            }
        }
        nap(15);
    }
    return 0;
}

/* 暫停期間的互動，含翻頁。
 *
 * 翻頁之後要「留在暫停」，所以這裡是個迴圈：paused_loop() 回報翻頁就解碼
 * 並顯示，然後再進 paused_loop() 等下一個指令，直到使用者要繼續或回選單。
 *
 * 先前這段邏輯散在 wait_interval 和 slideshow 兩處，而 wait_interval 只判斷
 * 「是不是回選單」，翻頁的回傳值被當成沒事發生 —— 症狀就是滑動翻了頁卻
 * 立刻自動繼續播放。集中在一個地方就不會再漏。
 *
 * *cur 進來是目前顯示的索引，出去是最後顯示的那張。
 * 回傳 0 = 繼續播放，1 = 回選單。 */
static int pause_session(int32_t *cur)
{
    g_pause_flash = true;
    for (;;) {
        int a;

        /* 疊加層的拉桿要顯示目前是第幾張。 */
        g_ov_index = (uint32_t)((*cur < 0) ? 0 : *cur);

        /* 愛心的狀態要在畫疊加層之前算好。清單在記憶體裡，純查表不碰卡片。 */
        {
            const char *p = cur_photo_path();

            if (p == NULL || !fav_ready()) {
                g_ov_fav = FAVBTN_LOCKED;
            } else {
                g_ov_fav = fav_is(p) ? FAVBTN_ON : FAVBTN_OFF;
            }
        }

        a = paused_loop();
        int dir;
        uint32_t target, tries;
        photo_result_t r;

        if (a == PAUSE_MENU) {
            return 1;
        }
        if (a == PAUSE_RESUME) {
            g_resume_ms = HAL_GetTick();
            return 0;
        }
        if (a == PAUSE_SEEK) {
            /* 放開才解那一張。停在暫停，讓使用者可以接著再拖或翻頁。 */
            uint32_t t = g_seek_target;

            if (t < g_order_count &&
                photo_show(PLAYLIST_BASE + g_order[t] * PATH_MAX)
                    == PHOTO_OK) {
                present();
                *cur = (int32_t)t;
                note_decode(PHOTO_OK);
            }
            continue;
        }

        if (a == PAUSE_ROTATE) {
            /* 自動 <-> 固定，不再三段循環。離開自動時凍結在**目前這張的
             * 方向** —— 使用者看到什麼就固定什麼，畫面不會按一下就翻掉。 */
            if (photo_get_orientation() == PHOTO_ORIENT_AUTO) {
                photo_set_orientation(gfx_is_landscape()
                                      ? PHOTO_ORIENT_LANDSCAPE
                                      : PHOTO_ORIENT_PORTRAIT);
            } else {
                photo_set_orientation(PHOTO_ORIENT_AUTO);
            }
            /* 重畫目前這張，讓使用者當場看到新方向的結果。 */
            if (*cur >= 0 &&
                photo_show(PLAYLIST_BASE + g_order[*cur] * PATH_MAX)
                    == PHOTO_OK) {
                present();
            }
            flash_orient();
            continue;
        }

        /* 翻頁：往指定方向找一張解得開的，最多繞一圈。 */
        dir    = (a == PAUSE_PREV) ? -1 : 1;
        target = (uint32_t)((*cur < 0) ? 0 : *cur);
        tries  = 0;
        r      = PHOTO_ERR_READ;

        while (tries < g_order_count && !g_card_sick) {
            watchdog_feed();
            target = (target + g_order_count + (uint32_t)dir) % g_order_count;
            r = photo_show(PLAYLIST_BASE + g_order[target] * PATH_MAX);
            tries++;
            if (r == PHOTO_OK || r == PHOTO_ABORTED) {
                break;
            }
            note_decode(r);
        }
        if (r == PHOTO_OK) {
            present();
            *cur = (int32_t)target;
            note_decode(PHOTO_OK);
        }
    }
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

    BBOX[13]++;

    wait_release();

    for (;;) {
        photo_result_t r;
        uint32_t t0;

        watchdog_feed();

        /* 連續解不開太多張 —— 卡片是好的，是照片本身的格式不對。
         * **一定要在螢幕上說話**：不講的話畫面就只是不動，看起來跟當機
         * 一模一樣（相簿在幾十毫秒內一張張失敗，沒有東西可換）。
         * 使用者實際踩過，而相框放在桌上時沒有人會接 SWD 去讀計數器。 */
        /* 連續全部失敗 -> 這個資料夾整個不能播，停下來回選單。
         * 門檻取 min(清單大小, 8)：只有一兩張的資料夾失敗一次就等於全滅，
         * 寫死 8 的話永遠湊不到（使用者用「一張不支援的圖」測就踩到了）。 */
        if (g_bad_photo_run >= ((g_order_count < BAD_PHOTO_RUN)
                                ? g_order_count : BAD_PHOTO_RUN)) {
            g_bad_photo_run = 0;
            show_message("這些照片解不開",
                         "需要 baseline JPEG、單張 2MB 以內");
            HAL_Delay(4000);
            return;              /* 回選單，不要繼續空轉 */
        }

        /* 只有一兩張壞掉：**提示但不打斷**，而且整次播放只講一次。
         * 不講的話使用者永遠不知道有照片放不出來；每輪都講的話，
         * 四千多張裡混進一張壞檔就會讓相框變成警報器。 */
        if (g_bad_photo_run > 0u && !g_bad_photo_told) {
            g_bad_photo_told = true;
            show_message("有照片解不開，已跳過",
                         "需要 baseline JPEG、單張 2MB 以內");
            HAL_Delay(2000);
        }

        if (!sd_present() || g_card_sick || !wait_screen_on()) {
            return;
        }

        /* 解下一張：畫進 back buffer，此時前一張還在螢幕上。 */
        t0 = HAL_GetTick();
        r = photo_show(PLAYLIST_BASE + g_order[pos] * PATH_MAX);
        if (r != PHOTO_OK && r != PHOTO_ABORTED) {
            r = photo_show(PLAYLIST_BASE + g_order[pos] * PATH_MAX);
        }
        g_last_ms = HAL_GetTick() - t0;

        if (r == PHOTO_ABORTED) {
            /* 解碼途中按了暫停：這張不要了，停在螢幕上那張。
             * 半畫好的 back buffer 沒有 present，看不到。 */
            if (pause_session(&cur)) {
                return;
            }
            pos = (uint32_t)((cur < 0) ? 0 : (cur + 1)) % g_order_count;
            continue;
        }

        if (r == PHOTO_OK) {
            note_decode(PHOTO_OK);

            if (cur >= 0) {
                int w = wait_interval(g_last_ms);

                if (w == 1 || w == 2) {
                    return;             /* 回選單或卡片被拔掉 */
                }
                if (w == 3) {
                    if (pause_session(&cur)) {
                        return;
                    }
                    /* 暫停期間可能翻過頁，接著要播的是目前這張的下一張。
                     * back buffer 裡預解的那張作廢，重新解。 */
                    pos = (uint32_t)(cur + 1) % g_order_count;
                    continue;
                }
            }
            present();
            cur = (int32_t)pos;
        } else {
            /* 一張壞掉的照片不能讓相框停住，記錄之後直接換下一張。 */
            note_decode(r);
        }

        pos++;
        if (pos >= g_order_count) {
            build_order();              /* 播完一輪重新洗牌 */
            pos = 0;
        }
    }
}

/* ------------------------------------------------------------------ */

/* 卡片還插著但停止回應時的復原。
 *
 * 軟體重置**不會**切斷 SD 卡電源，所以光是重開機救不回來（README 記過
 * 這條）。要靠 BSP_SD_DeInit() 把 SDMMC 斷電、偵測腳 DeInit 掉，
 * 下一次 disk_initialize 才會從乾淨的狀態重跑完整的 BSP_SD_Init。
 *
 * 這是「卡片不回應」與「卡片被拔掉」共用的復原路徑 —— 差別只在前者
 * 使用者不必動手。 */
static void card_recover(void)
{
    g_dbg_card_recover++;
    show_message("記憶卡沒有回應", "重新初始化中");

    f_mount(NULL, g_drive, 0);          /* 丟掉 FatFs 的快取 */
    (void)BSP_SD_DeInit(0);
    for (int i = 0; i < 10; i++) {      /* 分段等，中間要餵狗 */
        watchdog_feed();
        HAL_Delay(100);
    }

    g_read_fail_run = 0;
    g_card_sick     = false;            /* 外層迴圈會重新掛載並掃描 */
}

/* 掛載並掃描一次。回傳 false 代表這輪不能播（沒卡、掛不起來、沒照片）。 */
static bool mount_and_scan(void)
{
    BBOX[8]++;                      /* 一輪開機裡進來很多次 = 在重掃迴圈裡 */
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

    /* 掛好了才能讀清單檔。放在掃描之前，這樣選單畫出來就知道有幾張最愛。 */
    fav_init(g_drive);

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
    {   /* 掃描耗時（毫秒）。使用者反映拔線後等太久，先量準再談怎麼改。 */
        uint32_t t0 = HAL_GetTick();
        scan(0);
        g_scan_ms = HAL_GetTick() - t0;
    }
    watchdog_feed();

    /* 測試捲動用：假裝有這麼多資料夾。**只存在記憶體**，不會動到卡片，
     * 重開機就沒了。不寫（維持 0）就完全等於不存在。 */
    if (g_dbg_fakedirs > g_top_count && g_dbg_fakedirs <= MAX_TOP) {
        for (uint32_t i = g_top_count; i < g_dbg_fakedirs; i++) {
            (void)snprintf(g_top[i].name, NAME_MAX, "測試資料夾 %u",
                           (unsigned)(i + 1u));
            g_top[i].photos   = 0;
            g_top[i].selected = false;
        }
        g_top_count = g_dbg_fakedirs;
    }

    /* 影片只掃根目錄，很快。放在照片掃描之後，這樣選單畫出來時就知道
     * 要不要顯示「影片」按鈕。 */
    scan_videos();

    /* 測試影片清單捲動用。跟 g_dbg_fakedirs 一樣**只存在記憶體**，
     * 假的那幾部 count = 0，點下去 video_open() 會被檔頭檢查擋掉。 */
    if (g_dbg_fakevids > g_vid_count && g_dbg_fakevids <= MAX_VIDEOS) {
        for (uint32_t i = g_vid_count; i < g_dbg_fakevids; i++) {
            (void)snprintf(g_vids[i].name, VIDEO_NAME_MAX, "測試影片 %u",
                           (unsigned)(i + 1u));
            g_vids[i].path[0]  = 0;
            g_vids[i].count    = 0;
            g_vids[i].fps_x100 = 2400u;
        }
        g_vid_count = g_dbg_fakevids;
    }
    watchdog_feed();

    BBOX[11] = g_photo_count;
    /* 影片的順序可能跟上次不一樣，記住的位置就不能再用了。 */
    memset(g_vid_resume, 0, sizeof(g_vid_resume));

    /* 只有影片沒有照片也算可用 —— 使用者可能就是拿它當影片播放器。 */
    if (g_photo_count == 0U && g_vid_count == 0U) {
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
        nap(100);
    }

    /* 插入的瞬間接點會彈跳，等它穩定再動卡片，否則初始化容易失敗。 */
    for (int i = 0; i < 10; i++) {
        watchdog_feed();
        nap(100);
    }
    show_message("讀取記憶卡", "載入中");
}

/* 從卡片根目錄找第一個 .wav。**不要寫死檔名** —— 交接文件寫的是
 * MOVIE10.wav，實際掃到的影片卻叫 video1.bin，猜錯就是白跑一輪燒錄。 */
/* 影片的音軌是**同名的 .wav**（`video1.bin` -> `video1.wav`）。
 *
 * 用配對而不是「根目錄第一個 .wav」：卡上會有好幾部影片，之後做音畫同步時
 * 必須確定聲音就是這一部的，抓錯一部的話畫面與聲音從第一秒就對不起來，
 * 而那個症狀看起來會像「同步演算法寫錯了」。 */
static bool wav_path_of(const char *video_path, char *out, size_t cap)
{
    size_t n = strlen(video_path);
    FILINFO fno;

    if (n < 5u || n + 1u > cap) { return false; }
    memcpy(out, video_path, n + 1u);

    /* 換掉副檔名（.bin -> .wav）。找最後一個點，而且不能跨過目錄分隔符。 */
    while (n > 0u && out[n - 1u] != '/' && out[n - 1u] != '.') { n--; }
    if (n == 0u || out[n - 1u] != '.' || n + 3u >= cap) { return false; }
    out[n] = 'w'; out[n + 1u] = 'a'; out[n + 2u] = 'v'; out[n + 3u] = 0;

    return f_stat(out, &fno) == FR_OK;
}

/* 第二步的驗證：從卡上串流一整個 WAV。
 *
 * 只驗音訊，不碰影片 —— 一次只加一個沒驗證過的東西（board-notes 第八章）。
 * 欠載次數（g_dbg_wav_under）比「聽起來有沒有卡」可靠得多。 */
/* volume 是 0~100。請求裡帶得進來（0x5A5B00nn），這樣換音量不必重編、
 * 重燒一輪 —— 音量是要用耳朵判斷的東西，來回會很多次。 */
static void wav_test(uint32_t volume)
{
    char path[PATH_MAX];

    /* 挑**第一部有音軌的影片**，不是第一部影片。卡上大部分影片沒有 .wav，
     * 寫死第 0 部只會得到「找不到音軌」，而那看起來會像配對邏輯壞掉。 */
    {
        uint32_t i;
        bool     ok = false;

        for (i = 0; i < g_vid_count; i++) {
            if (wav_path_of(g_vids[i].path, path, sizeof(path))) {
                BBOX[125] = i;      /* 配到第幾部，之後同步要用同一部 */
                ok = true;
                break;
            }
        }
        if (!ok) {
            BBOX[112] = 0xE0000001u;
            show_message("找不到影片的音軌", "要有同名的 .wav");
            HAL_Delay(2000);
            return;
        }
    }
    show_message("音訊串流測試", path);

    BBOX[112] = audio_wav_start(path, volume) ? 1u : 0u;
    BBOX[126] = volume;
    BBOX[113] = g_dbg_wav_step;
    BBOX[114] = g_dbg_wav_rate;
    BBOX[115] = g_dbg_wav_fmt;
    BBOX[116] = g_dbg_wav_bytes;

    while (audio_wav_active()) {
        watchdog_feed();
        audio_wav_pump();
        BBOX[117] = g_dbg_wav_fed;
        BBOX[118] = g_dbg_wav_under;
        BBOX[119] = g_dbg_wav_rderr;
        if (!sd_present()) { break; }
        {   /* 點一下畫面就停，不必等十分鐘或按 reset。 */
            int x, y;
            if (read_touch(&x, &y)) { break; }
        }
    }
    audio_wav_stop();
    BBOX[117] = g_dbg_wav_fed;
    BBOX[118] = g_dbg_wav_under;
    BBOX[119] = g_dbg_wav_rderr;
}

/* ------------------------------------------------------------------ */
/* 最愛的自動測試                                                      */
/* ------------------------------------------------------------------ */

/* SWD 寫 g_dbg_favtest = N 就跑一輪：挑 N 張平均散布的照片收藏，
 * 再用最愛模式把它們全部播一遍。不寫就完全等於不存在
 * （board-notes 16.3：實驗要由外部旗標觸發）。
 *
 * 為什麼要做成韌體端的自動測試而不是用 SWD 一步一步戳：
 * 每次 SWD 連線要一秒多，戳十幾步就好幾十秒，而且**節奏跟使用者用手點
 * 完全不同** —— 這次的失敗正是連續操作幾次之後才出現的，
 * 慢慢戳反而重現不了。 */
volatile uint32_t g_dbg_ft_step;        /* 正在收藏第幾張 */
volatile uint32_t g_dbg_ft_added;       /* 收藏成功幾張 */
volatile int32_t  g_dbg_ft_err;         /* 第一次失敗的錯誤碼 */
volatile uint32_t g_dbg_ft_failat;      /* 第幾張開始失敗 */
volatile uint32_t g_dbg_ft_order;       /* 最愛模式建出幾張的播放順序 */
volatile uint32_t g_dbg_ft_shown;       /* 播成功幾張 */
volatile uint32_t g_dbg_ft_showfail;
volatile uint32_t g_dbg_ft_wr;          /* 整輪用掉幾次 disk_write */
volatile uint32_t g_dbg_ft_done;        /* 1 = 跑完 */

extern volatile uint32_t g_sd_writes;

static void fav_selftest(uint32_t n)
{
    uint32_t step, w0;

    if (n == 0u || g_photo_count == 0u) {
        return;
    }
    if (n > g_photo_count) { n = g_photo_count; }
    step = g_photo_count / n;
    if (step == 0u) { step = 1u; }

    g_dbg_ft_done = 0; g_dbg_ft_step = 0; g_dbg_ft_added = 0;
    g_dbg_ft_err  = 0; g_dbg_ft_failat = 0; g_dbg_ft_order = 0;
    g_dbg_ft_shown = 0; g_dbg_ft_showfail = 0;
    w0 = g_sd_writes;

    show_message("最愛自動測試", "收藏中");

    for (uint32_t i = 0; i < n && !g_card_sick; i++) {
        const char  *path = PLAYLIST_BASE + (i * step) * PATH_MAX;
        fav_result_t r;

        g_dbg_ft_step = i + 1u;
        watchdog_feed();
        r = fav_add(path);
        if (r == FAV_OK) {
            g_dbg_ft_added++;
        } else if (g_dbg_ft_err == 0) {
            g_dbg_ft_err    = (int32_t)r;
            g_dbg_ft_failat = i + 1u;
        }
        /* 模擬使用者用手點的節奏，不要背對背灌。 */
        nap(300);
    }

    show_message("最愛自動測試", "播放中");
    g_fav_mode = true;
    build_order();
    g_dbg_ft_order = g_order_count;

    for (uint32_t i = 0; i < g_order_count && !g_card_sick; i++) {
        watchdog_feed();
        if (photo_show(PLAYLIST_BASE + g_order[i] * PATH_MAX) == PHOTO_OK) {
            present();
            g_dbg_ft_shown++;
            note_decode(PHOTO_OK);
        } else {
            g_dbg_ft_showfail++;
            note_decode(PHOTO_ERR_DECODE);
        }
    }
    g_fav_mode = false;

    g_dbg_ft_wr   = g_sd_writes - w0;
    g_dbg_ft_done = 1u;
}

void album_run(void)
{
    TS_Init_t ts = { .Width = PHYS_W, .Height = PHYS_H,
                     .Orientation = TS_SWAP_NONE, .Accuracy = 2 };
    bool wdt_reset;

    g_stage = 1;

#if ALBUM_DCACHE_OFF
    SCB_DisableDCache();
#endif

    /* USB 無線耳機（CN17）。**失敗不影響相簿其他功能** —— 沒插 dongle
     * 是常態，這個呼叫只是把主機堆疊起起來等它出現。
     * 狀態看 DTCM 黑盒子 160 號之後（見 usbaudio.c）。 */
#if ALBUM_USB_AUDIO
    /* 麵包屑：釘死「usbaudio_init 到底有沒有回來」。
     * USB 的黑盒子顯示 USBH_Start 沒回來（缺 0x20 那個位元），但相簿
     * 明明跑到第 5 階段又滑了兩次 —— 兩者矛盾，代表我對其中一邊的
     * 判讀是錯的。不要再推，讓它自己說。 */
    BBOX[148] = 0xAA000001u;
    usbaudio_init();
    BBOX[148] = 0xAA000002u;
#endif

    /* 睡眠時保持除錯連線。
     *
     * nap() 用 __WFI() 讓核心睡到下一次中斷，省電效果好，但代價是核心一睡
     * 除錯介面的時脈就被關掉 —— SWD 會直接失聯，報
     * "Unable to read device id from ROM table"，連讀個變數都做不到。
     * （還救得回來：mode=UR 是連線時按住重置，核心來不及睡。）
     *
     * 這一位元讓睡眠時的除錯時脈保持開啟。代價是省電效果打折，但這台是
     * 開發板、隨時要用 SWD 讀狀態，值得。真的要壓到最低耗電時再拿掉。 */
    HAL_DBGMCU_EnableDBGSleepMode();

    /* **光是 DBG_SLEEP 不夠**，還要開除錯時脈。
     *
     * 實測：DBGMCU->CR 讀出來 0x10036485（DBG_SLEEP 確實是 1），但相簿停在
     * 選單時 —— 那個迴圈每圈 nap(30)，等於幾乎整段時間都在 __WFI() ——
     * mode=HOTPLUG 連續 15 次全部連不上，報 "Unable to get core ID"。
     * 只有 mode=UR（連線時按住重置，核心來不及睡）連得上。
     *
     * DBG_SLEEP 只是「睡著時不要把除錯模組關掉」，DBGCKEN 才是給除錯模組
     * 時脈的那一個，重置後預設是 0。兩個都要。 */
    DBGMCU->CR |= DBGMCU_CR_DBGCKEN;

    /* 開啟週期計數器，rnd_mix() 拿它當高解析度的熵來源。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;

    /* 量 CN18 的 VBUS，用來判斷 USB 線插到電腦了沒（見 vbus.c）。
     * 失敗不影響相簿：vbus_mv() 之後一律回 0，就是「永遠沒插線」。 */
    vbus_init();
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* 上一輪是被看門狗打掉的嗎？旗標要在這裡讀，之後會被清掉。
     * 整個 RSR 也一起存進黑盒子 —— 只記「是不是看門狗」分不出
     * 「軟體重置」（usbdrive_request_switch 會呼叫 NVIC_SystemReset）
     * 與「按了 reset」與「掉電」，而這三者要用完全不同的方向去查。 */
    bbox_boot(RCC->RSR);
    wdt_reset = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != 0u);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    psram_mpu_init();
    screen_init();

    if (BSP_TS_Init(0, &ts) != BSP_ERROR_NONE) {
        show_message("觸控初始化失敗", 0);
    }
    button_init();
    photo_set_abort_check(ctrl_waiting);

    /* 初始化過程中設定腳位會產生假的邊緣事件，清掉再開始。 */
    g_req_pause = 0u;
    g_pressing  = 0u;

    g_bright_idx = 0u;
    brightness_set(BRIGHT_STEP[0]);   /* 100%，並給 LPTIM 明確初值 */

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

    /* 開機時的音訊測試：請求放在黑盒子第 16 格（DTCM）。
     *
     * 為什麼不是用 g_dbg_audiotest：那個變數在 .bss，要用 SWD 寫它就得在
     * **執行中**連上板子，而 mode=HOTPLUG 現在完全連不上。在重置點寫的話
     * 又會被啟動碼清 .bss 的時候一起清掉（實測過，旗標寫進去就不見了）。
     * DTCM 兩邊都不吃虧：重置點寫得到、啟動碼也不會清它。
     *
     * 放在這裡是因為顯示與觸控（含 I2C）已經好了，而卡片還沒開始掛 ——
     * 音訊跟卡片是兩件獨立的事，先驗音訊就不必等掃卡。
     * 不寫請求就完全等於不存在（board-notes 16.3 的旗標觸發原則）。 */
    /* 0x5A5C = 測試音 + 自動播影片（量音訊 DMA 對輪詢解碼的干擾，A 組）
     * 0x5A5D = 只播影片（B 組對照）。**同一份韌體用旗標切換**，
     *          重編一版再比會混進別的差異。 */
#if ALBUM_AUTOPLAY
    g_dbg_autovideo = 2u;       /* 除錯建置：開機就播第一部有音軌的影片 */
#endif

    if ((BBOX[16] >> 16) == 0x5A5Cu || (BBOX[16] >> 16) == 0x5A5Du) {
        g_dbg_autovideo = 2u;   /* 挑有音軌的那一部 */
        BBOX[16] = ((BBOX[16] >> 16) == 0x5A5Du)
                 ? 0u                                     /* B 組：不播音 */
                 : (0x5A5A0000u | (BBOX[16] & 0xFFFFu));  /* A 組：接著播測試音 */
    }

    if ((BBOX[16] >> 16) == 0x5A5Au && (BBOX[16] & 0xFFFFu) <= 20000u) {
        uint32_t hz = BBOX[16] & 0xFFFFu;

        BBOX[16] = 0u;              /* 吃掉，不會自己重跑 */
        for (uint32_t i = 17u; i <= 25u; i++) { BBOX[i] = 0u; }  /* 清舊結果 */
        show_message("音訊測試", "耳機孔 CN16");
        if (audio_init(48000u, 90u)) {   /* 40 太小聲，實測聽不到 */
            (void)audio_tone(hz);
        }
        bbox_audio_snapshot();
    }

    g_stage = 30;
    BBOX[26] = 2u;              /* 音訊那段走完了，準備掛磁碟驅動 */
    if (FATFS_LinkDriver(&SD_BSP_Driver, g_drive) != 0) {
        show_message("磁碟介面初始化失敗", 0);
        for (;;) { }
    }

    /* 被看門狗打掉，代表上一輪卡在記憶卡裡出不來。
     *
     * 原本是「停住並要求使用者拔卡」，因為以為只有真正斷電才清得掉。
     * 但 BSP_SD_DeInit() 就會把 SDMMC 斷電、偵測腳 DeInit 掉，效果一樣 ——
     * 而且不必人在旁邊。相框放在桌上自己卡死、要人來拔卡才會動，
     * 是最糟的失敗模式。
     *
     * 先自己試一次；真的救不回來，下面的 mount_and_scan() 會失敗並顯示
     * 「記憶卡無法讀取」，那是個安靜等待的狀態，不會一直重置。 */
    if (wdt_reset) {
        show_message("記憶卡沒有回應", "重新初始化中");
        (void)BSP_SD_DeInit(0);
        HAL_Delay(1000);
    }

    watchdog_start();
    BBOX[26] = 4u;              /* 看門狗起來了，之後掛住會被自動重置 */

    /* 磁碟層等卡片的迴圈最長會佔住好幾秒（寫入逾時是 2 秒、還會重試），
     * 而看門狗只有 16 秒。第一版沒接這條，卡片一拒絕寫入就被重開，
     * 開機看到 IWDGRST 又停在「請拔出記憶卡再重新插入」—— 使用者看到的
     * 是「加入最愛失敗，然後連播放跟返回都不行」。 */
    sd_bsp_set_keepalive(watchdog_feed);

    g_stage = 3;
    BBOX[26] = 5u;              /* 準備進掛載/掃卡的主迴圈 */
    show_message("讀取記憶卡", "載入中");

    for (;;) {
        watchdog_feed();

        if (!sd_present()) {
            wait_for_card();
            continue;
        }
        if (!mount_and_scan()) {
            /* 掛不起來或沒照片：等使用者換一張卡再試。
             *
             * 但除錯旗標在這裡也要看得到 —— 拿一張沒有照片的卡來做寫入
             * 測試是很正常的事（比對不同卡片時就是這樣），只在正常路徑上
             * 檢查的話那些卡永遠測不到。 */
            while (sd_present()) {
                watchdog_feed();
                if (g_dbg_wrtest) {
                    uint32_t n = g_dbg_wrtest;

                    g_dbg_wrtest = 0;
                    show_message("寫入壓力測試", "進行中");
                    fav_stress(n);
                    continue;
                }
                if (g_dbg_cardreinit) {
                    g_dbg_cardreinit = 0;
                    break;              /* 跳出去重新掛載 */
                }
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
                note_decode(PHOTO_OK);
                present();
            } else {
                note_decode(PHOTO_ERR_DECODE);
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
        {
            bool first = true;

            while (sd_present() && !g_card_sick) {
                /* SWD 寫 g_dbg_favtest = N 就跑一輪最愛的自動測試。
                 * 選單迴圈看到旗標會先退出來，所以隨時觸發得到。 */
                if (g_dbg_favtest) {
                    uint32_t n = g_dbg_favtest;

                    g_dbg_favtest = 0;      /* 吃掉旗標，不會自己重跑 */
                    fav_selftest(n);
                    first = false;
                    continue;
                }
                /* 音訊第一步的驗證：SWD 寫 g_dbg_audiotest = 440 就播 A4。
                 * 只證明 BSP + I2S6 + WM8904 這條鏈路通不通，不碰 SD。 */
                if (g_dbg_audiotest) {
                    uint32_t hz = g_dbg_audiotest;

                    g_dbg_audiotest = 0;
                    g_dbg_aud_step |= AUD_STEP_TRIGGER;

                    show_message("音訊測試", "耳機孔 CN16");
                    if (audio_init(48000u, 40u)) {
                        (void)audio_tone(hz);
                    }
                    bbox_audio_snapshot();      /* 結果要活過下一次重置 */
                    first = false;
                    continue;
                }

                /* 音訊第二步：串流卡上的 WAV。請求放 DTCM（見 bbox_boot），
                 * 標籤跟測試音分開 —— 這一個需要卡片已經掛好，所以不能在
                 * 開機那條路上做。 */
                if ((BBOX[16] >> 16) == 0x5A5Bu) {
                    uint32_t vol = BBOX[16] & 0xFFFFu;

                    BBOX[16] = 0u;
                    wav_test((vol == 0u || vol > 100u) ? 40u : vol);
                    first = false;
                    continue;
                }

                /* 改完 g_dbg_sdclkdiv 之後用這個讓新時脈生效，不必重燒。 */
                if (g_dbg_cardreinit) {
                    g_dbg_cardreinit = 0;
                    g_card_sick = true;     /* 讓下面的 card_recover 收尾 */
                    break;
                }
                /* 純寫入壓力測試：只量卡片，完全不碰最愛的邏輯。 */
                if (g_dbg_wrtest) {
                    uint32_t n = g_dbg_wrtest;

                    g_dbg_wrtest = 0;
                    show_message("寫入壓力測試", "進行中");
                    fav_stress(n);
                    first = false;
                    continue;
                }
                /* 自動開播：掃描完直接播，不必手動勾資料夾按開始。
                 *
                 * 資料夾本來就預設全選（見掃描那段），所以自動開播等於
                 * 「什麼都不做就會動」。從播放往下滑回到選單之後，
                 * 後續就走正常的手動流程 —— 想改設定隨時進得去。 */
                if (AUTO_PLAY && first && selected_photo_count() > 0u) {
                    first = false;
                    build_order();
                    slideshow();
                    continue;
                }
                first = false;
                /* 除錯用：SWD 寫 g_dbg_autovideo 非 0 就直接播第一部影片。
                 * 影片問題只能靠點螢幕重現，這個旗標讓遠端也追得動。
                 * 不寫就完全等於不存在（board-notes 16.3 的旗標觸發原則）。 */
                if (g_dbg_autovideo && g_vid_count > 0u) {
                    BBOX[127] = autovideo_index();
                    g_dbg_autovideo = 0u;       /* 一次性，見選單那一處的說明 */
                    play_video(&g_vids[BBOX[127]]);
                    continue;
                }
                if (select_screen()) {
                    slideshow();
                }
            }
            if (g_card_sick) {
                card_recover();
            }
        }
#endif
        /* 跳出來代表卡片被拔掉了，或是它不再回應（card_recover 已處理）。 */
    }
}
