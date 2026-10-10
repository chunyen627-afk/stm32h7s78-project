/**
 * 賽車儀表：SimHub 經 ST-LINK 的虛擬 COM 埠（UART4）送遙測，畫在 7 吋螢幕上。
 *
 * 資料格式是 DSH1（沿用 caa1211/esp32-gt7-dashboard，MIT），SimHub 那邊用
 * Custom Serial devices 外掛、公式在 projects/racedash/simhub/。
 *
 * 螢幕初始化、雙緩衝、垂直消隱換頁照搬 projects/tetris 驗證過的做法
 * （為什麼這樣寫的說明在那邊的 game_main.c），差別是這裡用橫向 800x480。
 */
#include "main.h"
#include "stm32h7s78_discovery.h"
#include "stm32h7s78_discovery_lcd.h"
#include "stm32h7s78_discovery_ts.h"

#include "gfx.h"
#include "dsh1.h"
#include "dash.h"
#include "serial.h"

#include <string.h>

void dma2d_init(void);
void dma2d_wait(void);
void dma2d_fill(uint16_t *dst, int px, int py, int pw, int ph, uint16_t color);

#define FB0_ADDR   LCD_LAYER_0_ADDRESS   /* 0x90000000，PSRAM */
#define FB1_ADDR   LCD_LAYER_1_ADDRESS   /* 0x90200000 */

#define SERIAL_BAUD   115200u
#define STALE_MS      2000u     /* 這麼久沒收到有效的一行就回等待畫面 */

/* 狀態放 DTCM，重置不會洗掉，用 SWD 讀得到（相簿黑盒子同一個位置，
 * 兩個專案不會同時在板子上）。[0] 魔術字 "RDSH"、[1] tick、[2] 收到的位元組、
 * [3] 有效行、[4] 無效行、[5] 畫了幾格、[6] 最後的序號、[7] 一格畫多少週期、
 * [8] 序列埠溢位、[9] 太長被丟的行、[11] 展示模式、[12] SWD 切頁請求、[13] 觸控切頁次數、
 * [14] 目前頁面（0x9A6E0000 | 頁），開機時沿用 —— 重置後還記得選哪頁、
 *      斷電才回第一頁（DTCM 斷電是亂數，所以要有識別碼） */
#define BOX ((volatile uint32_t *)0x20004020u)

static uint32_t g_front;

#define PAGE_TAG    0x9A6E0000u
#define BADGE_MS    1200u       /* 切頁後名稱顯示多久 */
#define TOUCH_MS    40u         /* 觸控輪詢間隔（每格都問 I2C 沒必要） */
#define LONG_MS     700u        /* 按住這麼久叫出選單 */
#define MENU_MS     8000u       /* 選單沒動作多久自己關 */

/* 展示模式：SWD 寫 BOX[11] = "DEMO"（0x44454D4F）就由板子自己產生跑圈資料，
 * 不用開遊戲、也不用佔 COM 埠（SimHub 開著時電腦端的 fake_simhub.ps1 用不了）。
 * 動作跟 fake_simhub.ps1 一樣：加速換檔、偶爾煞車。 */
#define DEMO_MAGIC  0x44454D4Fu

static void demo_fill(dsh1_t *d, uint32_t now)
{
    static uint32_t last;
    static int32_t  rpm = 3000;
    static int      gear = 1;
    uint32_t        t = now % 12000u;

    if (now - last < 100u) { return; }      /* 跟 SimHub 一樣一秒 10 次 */
    last = now;
    if (t < 9000u) {
        d->throttle = 100; d->brake = 0;
        rpm += 900;
        if (rpm > 8200 && gear < 6) { gear++; rpm = 5600; }
        if (rpm > 8400) { rpm = 8400; }
    } else {
        d->throttle = 0; d->brake = 85;
        rpm -= 1200;
        if (rpm < 4200 && gear > 2) { gear--; rpm = 6800; }
        if (rpm < 2000) { rpm = 2000; }
    }
    d->seq++;
    d->running     = true;
    d->gear        = (char)('0' + gear);
    d->rpm         = rpm;
    d->rpm_pct     = rpm * 100 / 8500;
    d->redline_pct = 92;
    d->speed       = gear * 38 + (rpm - 2000) * 40 / 6500;
    d->lap_ms[0]   = (int32_t)(now % 84000u);
    d->lap_ms[1]   = 84631;
    d->lap_ms[2]   = 83912;
    d->fuel_pct    = 96;
    d->fuel_laps_x10 = 128;
    d->position    = 5;
    d->lap         = 3;
    d->laps_total  = 10;
    d->tc          = (t > 2000u && t < 2600u) ? 1 : 0;
    d->abs         = (t > 9400u && t < 10200u) ? 1 : 0;
    d->lap_invalid = 0;
    d->tyre_x10[0] = 784; d->tyre_x10[1] = 852;
    d->tyre_x10[2] = 961; d->tyre_x10[3] = 1093;
}

static bool touch_init(void)
{
    TS_Init_t ts;

    ts.Width       = PHYS_W;
    ts.Height      = PHYS_H;
    ts.Orientation = TS_SWAP_NONE;
    ts.Accuracy    = 2;
    return BSP_TS_Init(0, &ts) == BSP_ERROR_NONE;
}

/* 讀一次觸控。座標是面板座標（橫向 800x480，跟畫面一致）。 */
static bool touch_read(int *x, int *y)
{
    TS_State_t st;

    if (BSP_TS_GetState(0, &st) != BSP_ERROR_NONE || !st.TouchDetected) { return false; }
    *x = (int)st.TouchX;
    *y = (int)st.TouchY;
    return true;
}

static void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* framebuffer 設成 write-through：CPU 寫的東西要立刻落到 PSRAM，
 * LTDC 繞過快取直接讀（tetris 的 game_main.c 有完整說明）。 */
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
    gfx_set_orientation(true);                   /* 儀表是橫的：800x480 */
    gfx_set_framebuffer((uint16_t *)FB1_ADDR);
}

static void present(void)
{
    dma2d_wait();
    LTDC_Layer1->CFBAR = g_front ? FB0_ADDR : FB1_ADDR;
    LTDC->SRCR = LTDC_SRCR_VBR;
    for (uint32_t guard = 0; guard < 2000000u; guard++) {
        if ((LTDC->SRCR & LTDC_SRCR_VBR) == 0u) { break; }
    }
    g_front ^= 1u;
    gfx_set_framebuffer((uint16_t *)(g_front ? FB0_ADDR : FB1_ADDR));
}

void racedash_run(void)
{
    static char   line[512];
    static char   work[512];
    static dsh1_t tel;
    static dsh1_t demo;
    dash_link_t   link = {0};
    uint32_t      last_ok = 0;
    bool          have = false;

    int           page = 0;
    uint32_t      badge_until = 0, next_touch = 0;
    bool          have_touch;
    /* 觸控狀態機：放開才判斷是「點一下」；按住超過 LONG_MS 就開選單。
     * 電容觸控按著會偶爾漏報（board-notes 14.7），連續 3 次讀不到才算放開。 */
    bool          pressing = false, long_fired = false, menu = false;
    int           up_count = 0, press_x = 0, press_y = 0;
    uint32_t      press_t = 0, menu_until = 0;

    if ((BOX[14] & 0xFFFF0000u) == PAGE_TAG && (BOX[14] & 0xFFFFu) < DASH_PAGES) {
        page = (int)(BOX[14] & 0xFFFFu);
    }
    for (int i = 0; i < 16; i++) { BOX[i] = 0u; }
    BOX[0]  = 0x52445348u;                       /* "RDSH" */
    BOX[14] = PAGE_TAG | (uint32_t)page;

    cycle_counter_init();
    framebuffer_mpu_init();
    screen_init();
    dma2d_init();
    gfx_set_fill_hw(dma2d_fill);
    gfx_set_sync_hw(dma2d_wait);

    if (!serial_init(SERIAL_BAUD)) {
        Error_Handler();
    }
    have_touch = touch_init();   /* 觸控起不來也照樣顯示儀表，只是不能切頁 */

    for (;;) {
        uint32_t now = HAL_GetTick();
        int      n;


        while ((n = serial_read_line(line, sizeof(line))) >= 0) {
            if (n == 0) { continue; }
            memcpy(work, line, (size_t)n + 1u);  /* 解析會切開字串，留原文好除錯 */
            if (dsh1_parse(work, &tel)) {
                /* 還沒跑完一圈時 SimHub 把上一圈／最佳送成 0:00.000（EVO 實測），
                 * 畫成 -:--.--- 才不會像是跑了一圈零秒。目前這圈是 0 是合理的。 */
                if (tel.lap_ms[1] == 0) { tel.lap_ms[1] = DSH1_NA; }
                if (tel.lap_ms[2] == 0) { tel.lap_ms[2] = DSH1_NA; }
                link.lines_ok++;
                last_ok = now;
                have = true;
                BOX[6] = tel.seq;
            } else {
                link.lines_bad++;
            }
        }
        link.rx_bytes = g_serial_rx_bytes;

        /* 截圖用：SWD 寫 BOX[15] = "FROZ" 就停止重畫。從板子讀一整張畫面要
         * 將近一秒，期間儀表會重畫幾十次（每次先清黑再畫），讀到的是很多格
         * 混在一起的橫紋。凍住之後兩塊 framebuffer 都是完整的一格。 */
        if (BOX[15] == 0x5A4F5246u) {
            BOX[1] = now;          /* 序列埠照收（上面），只是不畫，不然緩衝會滿 */
            continue;
        }

        /* 除錯：SWD 寫 BOX[12] = 0x50470000 | 頁 就切到那一頁；寫 "MENU"（0x4D454E55）
         * 就叫出選單 —— 不用人去點螢幕也能截圖驗證。 */
        if (BOX[12] == 0x4D454E55u) {
            BOX[12] = 0u;
            menu = true;
            menu_until = now + MENU_MS;
        }
        if ((BOX[12] & 0xFFFF0000u) == 0x50470000u && (BOX[12] & 0xFFu) < DASH_PAGES) {
            page = (int)(BOX[12] & 0xFFu);
            BOX[12] = 0u;
            BOX[14] = PAGE_TAG | (uint32_t)page;
        }

        if (have_touch && (int32_t)(now - next_touch) >= 0) {
            int tx, ty;

            next_touch = now + TOUCH_MS;
            if (touch_read(&tx, &ty)) {
                up_count = 0;
                if (!pressing) {
                    pressing = true; long_fired = false;
                    press_t = now; press_x = tx; press_y = ty;
                } else if (!long_fired && !menu && (now - press_t) >= LONG_MS) {
                    long_fired = true;            /* 長按：開選單，放開時不再換頁 */
                    menu = true;
                    menu_until = now + MENU_MS;
                }
            } else if (pressing && ++up_count >= 3) {
                pressing = false;
                if (!long_fired) {
                    int pick = menu ? dash_menu_hit(press_x, press_y) : -1;

                    if (menu) {
                        menu = false;             /* 點到選項就套用，點空白處只關選單 */
                        if (pick >= 0) { page = pick; badge_until = now + BADGE_MS; }
                    } else {
                        page = (page + 1) % DASH_PAGES;
                        badge_until = now + BADGE_MS;
                    }
                    BOX[13]++;
                    BOX[14] = PAGE_TAG | (uint32_t)page;
                }
            }
            if (menu && (int32_t)(now - menu_until) >= 0) { menu = false; }
        }

        {
            uint32_t t0 = DWT->CYCCNT;

            if (BOX[11] == DEMO_MAGIC) {
                demo_fill(&demo, now);         /* 自己一份，不跟 SimHub 的真資料混在一起 */
                dash_draw(page, &demo, now);
            } else if (have && tel.running && (now - last_ok) < STALE_MS) {
                dash_draw(page, &tel, now);
            } else {
                dash_draw_waiting(&link, now);
            }
            if (menu) {
                dash_draw_menu(page);
            } else if ((int32_t)(badge_until - now) > 0) {
                dash_draw_page_badge(page);
            }
            BOX[7] = DWT->CYCCNT - t0;
        }
        present();

        BOX[1] = now;
        BOX[2] = link.rx_bytes;
        BOX[3] = link.lines_ok;
        BOX[4] = link.lines_bad;
        BOX[5]++;
        BOX[8] = g_serial_rx_overrun;
        BOX[9] = g_serial_line_dropped;
    }
}
