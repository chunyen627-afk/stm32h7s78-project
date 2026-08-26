/**
 * USB 無線耳機（USB Host / UAC1）——— 第一階段：讓它活起來。
 *
 * 說明見 usbaudio.h。底層的 LL 膠合在 usbh_glue.c。
 */
#include "usbaudio.h"

#include "main.h"          /* CMSIS 的暫存器定義與 HAL */
#include "usbh_core.h"
#include "usbh_audio.h"
#include "stm32h7rsxx_hal_hcd.h"

#include <string.h>

/* --- 觀察手段：DTCM 黑盒子 --------------------------------------------
 * 相簿沒有 UART（治具那邊有，因為 ST 的範例把訊息印到 UART4）。
 * 這裡沿用相簿既有的做法：把狀態寫進 DTCM，重置洗不掉，用 SWD 讀。
 * album_main.c 的黑盒子在 0x20004020，用到 147 號；USB 這邊從 160 開始，
 * 不會撞。
 *
 * **哨兵 0xFFFFFFFF = 這段根本沒跑到**，跟「跑了但結果是 0」分得開 ——
 * board-notes 8.7，而且我在治具那邊才因為沒守這條誤判過兩輪。 */
#define UBOX        ((volatile uint32_t *)0x20004020u)
#define UB_MAGIC    160   /* 0x55534231 "USB1"，證明這段程式真的執行過 */
#define UB_INIT     161   /* usbaudio_init 各步驟的位元圖 */
#define UB_STATE    162   /* 應用層狀態：0 閒置 1 已連接 2 類別就緒 3 斷線 */
#define UB_VIDPID   163   /* (VID << 16) | PID */
#define UB_PROC     164   /* usbaudio_process 呼叫次數（主迴圈驅動）*/
#define UB_GSTATE   165   /* USBH 核心的 gState */
#define UB_ALLOC    166   /* 靜態配置器用掉的最大位元組數 */
#define UB_ALLOCERR 167   /* 配置失敗次數（不是 0 就代表緩衝太小）*/
#define UB_ALT      168   /* (interface << 16) | alt setting */
#define UB_EPSIZE   169   /* 類別挑到的端點大小 */

#define UB_INIT_ENTER   (1u << 0)
#define UB_INIT_HOSTOK  (1u << 1)
#define UB_INIT_CLASSOK (1u << 2)
/* 開播要走一串非同步的控制傳輸，每一筆都要重試到 USBH_OK，所以是狀態機。
 * **順序不能換**：取樣率是設在「目前這個 alt 的端點」上的，而 alt 由類別
 * 在 ClassRequest 階段就設好了（按格式挑，見 patch_project.py）。 */
typedef enum {
    US_IDLE = 0,
    US_UNMUTE,      /* 解除靜音 —— ST 的類別從來不做這件事 */
    US_VOL1,        /* 音量：聲道 1 */
    US_VOL2,        /* 音量：聲道 2（主聲道常常只有靜音沒有音量）*/
    US_FREQ,        /* 取樣率 */
    US_PLAY,        /* USBH_AUDIO_Play */
    US_PLAYING,
    US_ENDED,
} usb_stream_state_t;

static volatile uint8_t s_state;   /* 初值 0 = US_IDLE */

#define UB_INIT_STARTOK (1u << 3)

/* --- 診斷（接在前面那批後面）--- */
#define UB_STREAM   170   /* 串流狀態機走到哪 */
#define UB_UNDER    171   /* 可播存量掉到 1/4 以下的次數（掏空的前兆）*/
#define UB_OVER     172   /* 類別衝出緩衝結尾的次數（應該永遠是 0）*/
#define UB_PUMPCNT  173   /* TIM7 中斷次數 */
#define UB_RDERR    174   /* f_read 失敗次數 */
#define UB_POSMS    175   /* 目前播放位置（毫秒）*/
/* --- 下面這幾格**刻意不在 init 清空** ------------------------------------
 * 用 SWD 讀 DTCM 必須 mode=UR，而 UR 會重置板子 —— 讀一次就洗掉一次現場。
 * 所以「當機那一輪」的證據要放在重置也不會被歸零的格子裡，跟相簿
 * BBOX[2]/[3]（上一輪最後的階段與 tick）同一個道理。
 *
 * 判讀方式：把 UB_TICK 跟相簿的 BBOX[3] 比 ——
 *   兩個都停在差不多的時間 -> 整個主迴圈停了
 *   BBOX[3] 一直在走而 UB_TICK 停住 -> 主迴圈活著但沒再進到音訊這條路 */
#define UB_TICK     176   /* 最後一次進到 usbaudio_wav_pump 的 HAL_GetTick */
#define UB_STARTS   177   /* start 呼叫累計 */
#define UB_STOPS    178   /* stop 呼叫累計 */
#define UB_SEEKS    179   /* seek 呼叫累計 */
#define UB_PAUSES   180   /* pause 呼叫累計 */
#define UB_NOTCLASS 181   /* 中斷進來時類別不在 HOST_CLASS 的次數 */
/* --- 把搜尋空間一刀切半的兩個麵包屑 ------------------------------------
 * 182：pump 被呼叫幾次。相簿的影片節奏是
 *      `while (next_ms > HAL_GetTick()) { audio_wav_pump(); }`，
 *      所以這個數字大得離譜 = 卡在那個忙等；沒在動 = 卡在別的地方。
 * 183：seek 走到哪一步。seek 是唯一可重現的觸發點，要能分辨它是
 *      「卡在 seek 裡面」還是「seek 做完之後才出事」。 */
#define UB_PUMPCALLS 182
#define UB_SEEKSTEP  183   /* 1 進入 2 已 Stop 3 已 lseek 4 已 prefill 5 完成 */
#define UB_STARTRET  184   /* USBH_Start 的實際回傳值 */
/* 181/182 原本要記 HardFault，但 HardFault_Handler 在 cube 的 it.c 已經有
 * 非弱定義，應用層覆寫不了 —— 要記就得改 it.c（patch_project.py 的工作）。
 * 先不做：NULL 檢查本來就該加，加完再看還當不當。 */


/* --- 靜態配置器 -------------------------------------------------------
 * USBH_malloc 在 usbh_conf.h 被導到這裡。**不用 malloc** 的理由寫在那邊：
 * 堆疊只有 1536 bytes，而且之後狀態機要進中斷。
 *
 * 類別只配置一次（AUDIO_HandleTypeDef），所以不需要真的做釋放 ——
 * 但要能分辨「沒配置過」與「配置失敗」，所以失敗有自己的計數。 */
static uint8_t  s_pool[3072] __attribute__((aligned(8)));
static uint32_t s_pool_used = 0u;

void *usbh_static_alloc(uint32_t size)
{
    uint32_t need = (size + 7u) & ~7u;

    if ((s_pool_used + need) > sizeof(s_pool)) {
        UBOX[UB_ALLOCERR]++;
        return NULL;
    }
    {
        void *p = &s_pool[s_pool_used];

        s_pool_used += need;
        if (s_pool_used > UBOX[UB_ALLOC]) {
            UBOX[UB_ALLOC] = s_pool_used;
        }
        return p;
    }
}

void usbh_static_free(void *p)
{
    (void)p;   /* 只配置一次，不必回收。 */
}

/* --- 主機控制代碼 ----------------------------------------------------- */
USBH_HandleTypeDef hUsbHostFS;
extern HCD_HandleTypeDef hhcd_USB_OTG_FS;

/* USB 改成純輪詢（NVIC 的 OTG 中斷是關掉的，見 usbh_glue.c 的說明）。
 * 這一個函式就是「服務一次 USB」：先處理硬體事件，再推狀態機。 */
static void usb_poll(void)
{
    HAL_HCD_IRQHandler(&hhcd_USB_OTG_FS);
    (void)USBH_Process(&hUsbHostFS);
}

/* 應用層看到的狀態。**volatile**：TIM7 的中斷之後也會改到相關狀態。 */
static volatile uint8_t s_appli = 0u;   /* 0 閒置 1 已連接 2 就緒 3 斷線 */
static volatile uint8_t s_inited = 0u;

/* 抽送權在誰身上：false = 主迴圈（nap）、true = TIM7 的中斷。
 * 定義放前面是因為 usbaudio_process() 要用到，而它在 pump_start 之前。 */
static volatile bool s_pump_on = false;

static AUDIO_HandleTypeDef *class_handle(void);   /* 定義在下面 */

static void usb_user_process(USBH_HandleTypeDef *phost, uint8_t id)
{
    switch (id) {
    case HOST_USER_DISCONNECTION:
        s_appli = 3u;
        /* **類別沒了就要立刻收攤。** 不收的話 pump 還會繼續呼叫
         * GetOutOffset / ChangeOutBuffer，而那些函式內部也會去
         * 解參照 pActiveClass。 */
        s_state = US_IDLE;
        s_pump_on = false;
        TIM7->CR1  = 0u;
        TIM7->DIER = 0u;
        break;

    case HOST_USER_CLASS_ACTIVE:
        s_appli = 2u;
        UBOX[UB_VIDPID] = ((uint32_t)phost->device.DevDesc.idVendor << 16) |
                          phost->device.DevDesc.idProduct;
        {
            AUDIO_HandleTypeDef *h = (phost->pActiveClass != NULL)
                ? (AUDIO_HandleTypeDef *)phost->pActiveClass->pData : NULL;

            if (h != NULL) {
                UBOX[UB_ALT] = ((uint32_t)h->headphone.interface << 16) |
                               h->headphone.AltSettings;
                UBOX[UB_EPSIZE] = h->headphone.EpSize;
            }
        }
        break;

    case HOST_USER_CONNECTION:
        s_appli = 1u;
        break;

    default:
        break;
    }
    UBOX[UB_STATE] = s_appli;
}

/* 二分法分級：
 *   1 = 只調 SysTick 優先權（其餘什麼都不做）
 *   2 = + USBH_Init（含 HAL_HCD_Init 與 MspInit：LSE/CRS/HSI48 時脈）
 *   3 = + USBH_RegisterClass
 *   4 = + USBH_Start（埠供電）
 * 症狀是「USB 初始化過就會讓相簿原本的 I2S seek 卡住」，
 * 而 seek 那條路跟 USB 毫無交集 —— 所以要找的是 init 動到的**共用資源**。 */
/*   5 = 只開 OTG 全域中斷，**不驅動 VBUS**
 *   6 = 只驅動 VBUS，**不開全域中斷**
 * level 2 通過、level 4 當機，而 USBH_Start 就只做這兩件事。 */
#define USBAUDIO_INIT_LEVEL  4

void usbaudio_init(void)
{
    /* 斷電後第一次才清累計欄。**用 SWD 讀 DTCM 必須 mode=UR，而 UR 會重置**
     * —— 每次讀就重置一次，若在這裡無條件清空，累計值永遠讀不到。
     * 但也不能完全不初始化：DTCM 斷電後是隨機內容，`UBOX[x]++` 會從垃圾
     * 開始加，看起來還是垃圾（我第一版就是這樣，讀回四個亂數）。
     * 相簿的黑盒子用同一招（BBOX[0] 的 magic）。 */
    if (UBOX[UB_MAGIC] != 0x55534231u) {
        UBOX[UB_TICK]   = 0u;
        UBOX[UB_STARTS] = 0u;
        UBOX[UB_STOPS]  = 0u;
        UBOX[UB_SEEKS]  = 0u;
        UBOX[UB_PAUSES] = 0u;
        UBOX[UB_NOTCLASS] = 0u;
        UBOX[UB_PUMPCALLS] = 0u;
        UBOX[UB_SEEKSTEP] = 0u;
    }

    UBOX[UB_MAGIC]    = 0x55534231u;
    UBOX[UB_INIT]     = UB_INIT_ENTER;
    UBOX[UB_STATE]    = 0u;
    UBOX[UB_VIDPID]   = 0xFFFFFFFFu;
    UBOX[UB_PROC]     = 0u;
    UBOX[UB_GSTATE]   = 0xFFFFFFFFu;
    UBOX[UB_ALLOC]    = 0u;
    UBOX[UB_ALLOCERR] = 0u;
    UBOX[UB_ALT]      = 0xFFFFFFFFu;
    UBOX[UB_EPSIZE]   = 0xFFFFFFFFu;

/* 這裡原本有一行 HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0)，
     * 是為了「USB 中斷餓死 SysTick」那個假設加的。那個假設後來被推翻
     * （改成輪詢之後根本沒有 USB 中斷），而且它自己成了新的嫌疑 ——
     * USBH_Start 卡在 HAL_Delay(200) 只有一個解釋：uwTick 不動。
     * 拿掉它，讓 SysTick 回到 HAL 的預設。 */

#if (USBAUDIO_INIT_LEVEL < 2)
    return;
#endif

    if (USBH_Init(&hUsbHostFS, usb_user_process, HOST_FS) != USBH_OK) {
        return;
    }
    UBOX[UB_INIT] |= UB_INIT_HOSTOK;

    /* **USBH_Init 一成功就開始服務。**
     * 原本是等到 USBH_Start 之後才設 s_inited，而中間任何一步出狀況
     * （或我對 INIT 位元的判讀出錯）都會讓 usbaudio_process() 一直空轉 ——
     * 那等於「OTG 中斷開著卻沒有人服務它」，實測就會把相簿卡死。
     * USBH_Process 在還沒 Start 的時候本來就是安全的（狀態機停在 IDLE）。 */
    s_inited = 1u;

#if (USBAUDIO_INIT_LEVEL < 3)
    return;
#endif

    if (USBH_RegisterClass(&hUsbHostFS, USBH_AUDIO_CLASS) != USBH_OK) {
        return;
    }
    UBOX[UB_INIT] |= UB_INIT_CLASSOK;

#if (USBAUDIO_INIT_LEVEL < 4)
    return;
#endif

    /* 麵包屑：INIT 讀到 7 但 USBH_Start 的實作是**無條件回傳 USBH_OK**，
     * 兩者矛盾 —— 所以我對這裡的模型一定有錯。不要再推，讓位元自己說。
     * 0x10 = 呼叫之前、0x20 = 呼叫回來了、0x40 = 判斷式也過了。 */
    UBOX[UB_INIT] |= 0x10u;
#if (USBAUDIO_INIT_LEVEL == 5)
    /* 只開全域中斷（HAL_HCD_Start 的一半）。VBUS 由 JP1 供，硬體上本來
     * 就有電，所以不驅動 PPWR 也還是接得上。 */
    __HAL_HCD_ENABLE((HCD_HandleTypeDef *)hUsbHostFS.pData);
    UBOX[UB_INIT] |= 0x20u;
    s_inited = 1u;
    UBOX[UB_INIT] |= UB_INIT_STARTOK;
    return;
#elif (USBAUDIO_INIT_LEVEL == 6)
    /* 只驅動 VBUS（HAL_HCD_Start 的另一半），不開全域中斷。 */
    (void)USB_DriveVbus(((HCD_HandleTypeDef *)hUsbHostFS.pData)->Instance, 1U);
    UBOX[UB_INIT] |= 0x20u;
    s_inited = 1u;
    UBOX[UB_INIT] |= UB_INIT_STARTOK;
    return;
#endif
    {
        USBH_StatusTypeDef st = USBH_Start(&hUsbHostFS);

        UBOX[UB_INIT] |= 0x20u;
        UBOX[UB_STARTRET] = (uint32_t)st;
        if (st != USBH_OK) {
            return;
        }
    }
    UBOX[UB_INIT] |= 0x40u;

#if (USBAUDIO_INIT_LEVEL == 4)
    /* 二分：照樣 USBH_Start（埠供電照做），但把 NVIC 的 OTG 中斷關掉。
     * 這樣就分得出「當機是供電造成的」還是「中斷造成的」。
     * 注意 OTG 中斷計數器讀到的是隨機值，代表它從來沒觸發過 ——
     * 所以我預期關掉它不會有差別；**如果有差別，就代表那個計數器在騙我**，
     * 那本身也是重要資訊。 */
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
    UBOX[UB_INIT] |= 0x80u;
#endif

    UBOX[UB_INIT] |= UB_INIT_STARTOK;

    s_inited = 1u;
}

void usbaudio_process(void)
{
    if (s_inited == 0u) {
        return;
    }

    /* **抽送權交出去之後主迴圈就不能再進來。**
     * 同一個狀態機被中斷與主迴圈同時跑會互相踩 —— 治具那邊有旗標擋，
     * 這裡第一版忘了加。s_pump_on 由 pump_start/stop 維護。 */
    if (s_pump_on) {
        return;
    }

    UBOX[UB_PROC]++;
    UBOX[UB_GSTATE] = (uint32_t)hUsbHostFS.gState;

    usb_poll();
}

bool usbaudio_ready(void)
{
    return (s_appli == 2u);
}

/* ======================================================================
 * 第二階段：串流後端
 *
 * 對外的形狀刻意跟 audio_out.h 的 7 個函式一樣，audio_out.c 在輸出切到
 * USB 時直接轉發過來。**I2S 那條路一個字都沒改**，只多了轉發的判斷。
 * ====================================================================== */
#include "wav_hdr.h"

/* 緩衝：192 x 160 = 30720 bytes = 160 毫秒。
 * **總長一定要整除 192**（一個 USB 訊框的位元組數）—— 類別是一次推進
 * 192 個位元組去繞回的，不整除的話繞回點會落在取樣框中間。
 * 160ms 跟相簿 I2S 那條路同量級（那邊實測 9.5 分鐘零欠載）。 */
#define USB_BLOCK      3072u
#define USB_BLOCKS     10u
#define USB_BUF_TOTAL  (USB_BLOCK * USB_BLOCKS)   /* 30720 = 192 x 160 */

static uint8_t  s_abuf[USB_BUF_TOTAL] __attribute__((aligned(32)));
static uint32_t s_in;                 /* 我們寫到哪 */

static FIL      s_file;
static bool     s_open;
static wav_hdr_t s_hdr;
static uint32_t s_byterate;           /* rate * ch * bits/8 */
static uint32_t s_left;               /* data 區塊還剩幾 bytes 沒讀 */
static uint32_t s_org;                /* 這一段的起點在 data 區塊裡的位移 */
static uint32_t s_sent0;              /* 起點那一刻類別的 global_ptr */
static uint8_t  s_vol_pct = 60u;
static volatile bool s_vol_dirty;     /* 音量變了，播放中要補寫 */
static uint8_t  s_vol_ch = 1u;        /* 補寫走到哪個聲道 */
static bool     s_paused;


/* --- TIM7：8kHz 抽送 ---------------------------------------------------
 * 為什麼不能靠主迴圈：USB 的等時端點要 1000 包/秒，而 USBH_Process 一次
 * 呼叫只送一包 —— 相簿每格影片要 17ms，主迴圈根本餵不動。
 * SOF（1kHz）也不行，實測只送得出 500 包/秒。
 * 完整的實測對照在 projects/usbaudio/README.md。
 *
 * HAL 的 TIM 模組沒開，直接寫暫存器。TIM7 是基本計時器，不佔腳位。
 * **優先權跟 OTG_FS 一樣**：同優先權不互相插隊，兩邊自然序列化。 */
/* --- 二分法開關（board-notes 8.2：二分法比推理快）---------------------
 * 0 = 完全不啟動 TIM7，USBH_Process 只由主迴圈的 nap() 驅動。
 * 這樣聲音會很差（相簿每格影片 17ms，餵不動 1000 包/秒），
 * 但可以一次判定「當機是不是中斷這一側造成的」。
 * 症狀是主迴圈與 TIM7 同時停、而且**沒有 CPU 故障**（故障處理常式已經
 * 會把 CFSR/BFAR 寫進黑盒子，實測那幾格是隨機內容）——
 * 那就是某個優先權 <= 6 的中斷不返回或中斷風暴。 */
#define USB_PUMP_ENABLE  0

#define USB_PUMP_HZ  8000u

static void stream_setup_step(void);   /* 定義在下面，中斷裡要用 */

static void pump_start(void)
{
    uint32_t timclk = HAL_RCC_GetPCLK1Freq() * 2u;   /* APB1 分頻不是 1 */

#if (USB_PUMP_ENABLE == 0)
    return;                       /* 二分法：抽送關掉，全部交給主迴圈 */
#endif
    s_pump_on = true;

    __HAL_RCC_TIM7_CLK_ENABLE();
    TIM7->CR1  = 0u;
    TIM7->PSC  = (timclk / 1000000u) - 1u;           /* 先分到 1MHz */
    TIM7->ARR  = (1000000u / USB_PUMP_HZ) - 1u;
    TIM7->EGR  = TIM_EGR_UG;
    TIM7->SR   = 0u;
    TIM7->DIER = TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM7_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1  = TIM_CR1_CEN;
}

static void pump_stop(void)
{
    s_pump_on = false;
    TIM7->CR1  = 0u;
    TIM7->DIER = 0u;
    HAL_NVIC_DisableIRQ(TIM7_IRQn);
}

void TIM7_IRQHandler(void)
{
    if ((TIM7->SR & TIM_SR_UIF) != 0u) {
        TIM7->SR = ~TIM_SR_UIF;
        UBOX[UB_PUMPCNT]++;

        /* --- **只在類別已經活著的時候才抽送** ---------------------------
         * USBH_Process 在列舉／重置那幾段會呼叫 USBH_Delay()，而它就是
         * HAL_Delay() —— 忙等 uwTick，而 uwTick 由 SysTick 中斷更新。
         * SysTick 的優先權是 15（最低），TIM7 是 6 —— **SysTick 插不進來，
         * uwTick 永遠不動，HAL_Delay 再也不會回來。**
         * 中斷不返回 = 主迴圈餓死 = 畫面凍住、觸控沒反應、連 PUMPCNT
         * 都停住。實測就是這個形狀，而且要撞上一次重新列舉才會發生
         * （所以「多播幾部影片才當」）。
         *
         * gState == HOST_CLASS 這條路徑裡沒有任何延遲，進中斷是安全的。
         * 其他狀態（列舉、類別請求、重置）留給主迴圈的 nap() 跑。 */
        if (hUsbHostFS.gState != HOST_CLASS) {
            UBOX[UB_NOTCLASS]++;
            return;
        }

        usb_poll();

        /* 開播序列也在這裡跑 —— 見 stream_setup_step() 的說明。 */
        if ((s_state != US_IDLE) && (s_state != US_ENDED) &&
            (s_state != US_PLAYING)) {
            stream_setup_step();
        }

        /* **繞回的所有權在這裡。** 留在主迴圈的話追不上（治具實測 over=225）
         * —— 類別送完整個緩衝之後 cbuf 會繼續往前走，沒人拉回來就會把
         * 緩衝後面的記憶體送出去。 */
        if (USBH_AUDIO_GetOutOffset(&hUsbHostFS) >= (int32_t)USB_BUF_TOTAL) {
            UBOX[UB_OVER]++;
            (void)USBH_AUDIO_ChangeOutBuffer(&hUsbHostFS, &s_abuf[0]);
        }
    }
}

/* --- 控制傳輸：解靜音與絕對音量 ---------------------------------------
 * 兩件 ST 的類別沒做對的事（治具那邊一個一個量出來的）：
 *   1. **它從來不解除靜音** —— `grep MUTE usbh_audio.c` 是空的。
 *      裝置若處於靜音，主機端會看到一個完美的串流而使用者什麼都聽不到。
 *   2. 它只提供相對的 VOLUME_UP/DOWN，而**裝置的開機預設值不固定**
 *      （同一支耳機實測 -58dB 與 -48dB 都出現過），所以「按 N 下」
 *      不是可重現的設定。這裡算絕對目標直接寫。
 *
 * 音量要寫**有那個控制的聲道**：Feature Unit 的 bmaControls[0]（主聲道）
 * 常常只有靜音沒有音量，寫過去或讀回來都沒有意義。
 *
 * Feature Unit 的編號從原始描述元推導（找輸出端子是喇叭/耳機那一族的，
 * 它的 bSourceID 就是），不要相信中介層的索引記帳。 */
static uint8_t s_fu = 0xFFu;
static uint8_t s_ctlbuf[2];

static void find_feature_unit(void)
{
    const uint8_t *raw = hUsbHostFS.device.CfgDesc_Raw;
    uint16_t have = hUsbHostFS.device.CfgDesc.wTotalLength;
    uint16_t i = 0u;

    s_fu = 0xFFu;
    while ((uint16_t)(i + 2u) <= have) {
        uint8_t blen = raw[i];

        if ((blen < 2u) || ((uint16_t)(i + blen) > have)) { break; }
        /* CS_INTERFACE(0x24) / OUTPUT_TERMINAL(0x03)，wTerminalType 高位 0x03
         * = 喇叭那一族（0x0301 喇叭 / 0x0302 耳機 / 0x0303 頭戴 ...） */
        if ((raw[i + 1u] == 0x24u) && (blen >= 9u) &&
            (raw[i + 2u] == 0x03u) && (raw[i + 5u] == 0x03u)) {
            s_fu = raw[i + 7u];                 /* bSourceID */
            return;
        }
        i = (uint16_t)(i + blen);
    }
}

static USBH_StatusTypeDef ctl_set(uint8_t selector, uint8_t chan,
                                  const uint8_t *val, uint8_t len)
{
    if (s_fu == 0xFFu) { return USBH_FAIL; }

    if (hUsbHostFS.RequestState == CMD_SEND) {
        s_ctlbuf[0] = val[0];
        if (len > 1u) { s_ctlbuf[1] = val[1]; }
        hUsbHostFS.Control.setup.b.bmRequestType =
            (uint8_t)(USB_H2D | USB_REQ_RECIPIENT_INTERFACE | USB_REQ_TYPE_CLASS);
        hUsbHostFS.Control.setup.b.bRequest  = UAC_SET_CUR;
        hUsbHostFS.Control.setup.b.wValue.w  =
            (uint16_t)(((uint16_t)selector << 8) | chan);
        hUsbHostFS.Control.setup.b.wIndex.w  = (uint16_t)((uint16_t)s_fu << 8);
        hUsbHostFS.Control.setup.b.wLength.w = len;
    }
    return USBH_CtlReq(&hUsbHostFS, s_ctlbuf, len);
}

/* 把 0~100 對映到裝置自己回報的 min..max。**dB 是對數刻度**，百分比只是
 * 給人調的把手，不要用它去推「應該多大聲」。 */
static int16_t vol_target(uint8_t pct)
{
    AUDIO_HandleTypeDef *h = class_handle();
    int32_t lo, hi;

    if (h == NULL) { return 0; }
    lo = (int16_t)h->headphone.attribute.volumeMin;
    hi = (int16_t)h->headphone.attribute.volumeMax;
    if (pct > 100u) { pct = 100u; }
    return (int16_t)(lo + (((hi - lo) * (int32_t)pct) / 100));
}

/* --- 檔案 -> 緩衝 ------------------------------------------------------ */
static volatile bool s_eof;

void USBH_AUDIO_BufferEmptyCallback(USBH_HandleTypeDef *phost)
{
    (void)phost;
    s_eof = true;      /* 唯一可信的「整段送完了」 */
}

static bool read_block(uint32_t at)
{
    UINT     got = 0;
    uint32_t want = USB_BLOCK;

    if (s_left < want) { want = s_left; }
    if (want == 0u) {
        memset(&s_abuf[at], 0, USB_BLOCK);       /* 尾巴補靜音 */
        return true;
    }
    if (f_read(&s_file, &s_abuf[at], want, &got) != FR_OK) {
        UBOX[UB_RDERR]++;
        return false;
    }
    if (got < USB_BLOCK) {
        memset(&s_abuf[at + got], 0, USB_BLOCK - got);
    }
    s_left -= got;
    return true;
}

static void prefill(void)
{
    uint32_t k;

    s_in = 0u;
    for (k = 0u; k < USB_BLOCKS; k++) {
        (void)read_block(k * USB_BLOCK);
    }
    s_in = 0u;
}

/* --- 取得類別控制代碼：**一定要檢查 pActiveClass** ---------------------
 * dongle 拔掉或重新列舉的時候 pActiveClass 會變成 NULL，直接
 * `pActiveClass->pData` 就是對 NULL 解參照 -> HardFault -> 整台凍住，
 * 連中斷都停（實測：主迴圈與 TIM7 在同一毫秒一起停）。
 * 「多播幾部影片才當」正是這個形狀 —— 要撞上一次重新列舉才會發生。 */
static AUDIO_HandleTypeDef *class_handle(void)
{
    if (hUsbHostFS.gState != HOST_CLASS) { return NULL; }
    if (hUsbHostFS.pActiveClass == NULL)  { return NULL; }
    return (AUDIO_HandleTypeDef *)hUsbHostFS.pActiveClass->pData;
}

static uint32_t class_sent(void)
{
    AUDIO_HandleTypeDef *h = class_handle();

    return (h != NULL) ? h->headphone.global_ptr : 0u;
}

bool usbaudio_wav_start(const char *path, uint32_t volume)
{
    usbaudio_wav_stop();

    if (!usbaudio_ready()) { return false; }

    UBOX[UB_UNDER] = 0u;
    UBOX[UB_OVER]  = 0u;
    UBOX[UB_RDERR] = 0u;

    UBOX[UB_STARTS]++;
    if (f_open(&s_file, path, FA_READ) != FR_OK) { return false; }
    s_open = true;

    if (!wav_hdr_parse(&s_file, &s_hdr)) { usbaudio_wav_stop(); return false; }

    /* 只吃 48kHz / 立體聲 / 16-bit。**不要嘗試轉換** —— 影片音軌一律是
     * 這個格式（video2bin.py 就是這樣輸出的），而且那是相容性最好的格式。
     * 不符就回 false，呼叫端會退回 I2S 或靜音，不會發出怪聲。 */
    if (s_hdr.rate != 48000u || s_hdr.channels != 2u || s_hdr.bits != 16u) {
        usbaudio_wav_stop();
        return false;
    }
    s_byterate = s_hdr.rate * s_hdr.channels * (s_hdr.bits / 8u);

    s_left    = s_hdr.data_len;
    s_org     = 0u;
    s_paused  = false;
    s_eof     = false;
    s_vol_pct = (uint8_t)((volume > 100u) ? 100u : volume);

    find_feature_unit();
    prefill();

    s_state = US_UNMUTE;
    UBOX[UB_STREAM] = s_state;
    pump_start();          /* 序列在中斷裡跑，所以現在就要啟動 */
    return true;
}

/* --- 開播序列：**在 TIM7 的中斷裡跑** ---------------------------------
 * 第一版放在 usbaudio_wav_pump()（主迴圈）裡，結果「播放要先暫停聲音才
 * 會出來」—— 因為**影片播放迴圈不經過 nap()**，`USBH_Process` 沒人跑，
 * 這串非同步的控制傳輸就停在半路；一暫停，暫停迴圈有 nap()，序列才走完。
 *
 * 搬進中斷同時解決兩件事：
 *   1. 序列不再依賴呼叫端的迴圈有多快
 *   2. **USB 的狀態機只有一個擁有者** —— 控制傳輸與 USBH_Process 在同一個
 *      執行環境，不會互相踩。治具那邊我就是敗在從主迴圈跟類別搶
 *      phost->Control，連續三輪量到三個不同的 alt。
 *
 * 檔案 I/O 留在主迴圈（FatFs 不能進中斷），見 usbaudio_wav_pump()。 */
static void stream_setup_step(void)
{
    static const uint8_t zero = 0u;

    switch (s_state) {

    case US_UNMUTE:
        if (ctl_set(MUTE_CONTROL, 0u, &zero, 1u) != USBH_BUSY) {
            s_state = US_VOL1;          /* 不支援就跳過，不要卡住 */
        }
        break;

    case US_VOL1:
    case US_VOL2: {
        int16_t t = vol_target(s_vol_pct);
        uint8_t v[2];

        v[0] = (uint8_t)((uint16_t)t & 0xFFu);
        v[1] = (uint8_t)(((uint16_t)t >> 8) & 0xFFu);
        if (ctl_set(VOLUME_CONTROL, (s_state == US_VOL1) ? 1u : 2u,
                    v, 2u) != USBH_BUSY) {
            s_state = (s_state == US_VOL1) ? US_VOL2 : US_FREQ;
        }
        break;
    }

    case US_FREQ:
        /* 取樣率是非同步控制傳輸，第一次一定回 USBH_BUSY —— 那不是因為
         * 傳輸還沒完成，是因為它要求 play_state 是 IDLE 而類別剛掛上時
         * 是 INIT。重試到 OK。 */
        if (USBH_AUDIO_SetFrequency(&hUsbHostFS, (uint16_t)s_hdr.rate,
                                    (uint8_t)s_hdr.channels,
                                    (uint8_t)s_hdr.bits) == USBH_OK) {
            s_state = US_PLAY;
        }
        break;

    case US_PLAY:
        /* USBH_AUDIO_Play 要求 play_state == IDLE，而 SetFrequency 只是把
         * 類別推進 SET_EP，那筆傳輸完成之後才回到 IDLE。**主動重試到 OK**
         * ——不要等 FrequencySet 回呼，那是一場撞不撞得上的競爭。 */
        if (USBH_AUDIO_Play(&hUsbHostFS, &s_abuf[0],
                            s_hdr.data_len - s_org) == USBH_OK) {
            s_sent0 = class_sent();
            s_state = US_PLAYING;
        }
        break;

    default:
        break;
    }
    UBOX[UB_STREAM] = s_state;
}

/* **從主迴圈呼叫。** 只做檔案 I/O（FatFs 不能進中斷）。
 * 開播序列與封包送出都在 TIM7 的中斷裡。 */
void usbaudio_wav_pump(void)
{
    UBOX[UB_TICK] = HAL_GetTick();
    UBOX[UB_PUMPCALLS]++;
    if (s_state != US_PLAYING) { return; }
    {
        int32_t out = USBH_AUDIO_GetOutOffset(&hUsbHostFS);
        int32_t diff;

        if (s_eof) { s_state = US_ENDED; return; }

        /* 播放中改音量：一次寫一個聲道，寫完就清旗標。
         * 兩個聲道要分開寫，而且中間不能插進別的控制傳輸 ——
         * 治具實測「前一步還沒做完就發下一步」會讓兩個聲道走散，
         * 聽起來就是左右換來換去。 */
        if (s_vol_dirty) {
            int16_t t = vol_target(s_vol_pct);
            uint8_t v[2];

            v[0] = (uint8_t)((uint16_t)t & 0xFFu);
            v[1] = (uint8_t)(((uint16_t)t >> 8) & 0xFFu);
            if (ctl_set(VOLUME_CONTROL, s_vol_ch, v, 2u) != USBH_BUSY) {
                if (s_vol_ch == 1u) {
                    s_vol_ch = 2u;
                } else {
                    s_vol_ch = 1u;
                    s_vol_dirty = false;
                }
            }
        }

        if (out < 0) { return; }        /* 還沒真的開始送 */

        diff = out - (int32_t)s_in;
        if (diff < 0) { diff += (int32_t)USB_BUF_TOTAL; }

        if ((uint32_t)((int32_t)USB_BUF_TOTAL - diff) < (USB_BUF_TOTAL / 4u)) {
            UBOX[UB_UNDER]++;           /* 存量掉到 1/4 以下 = 掏空的前兆 */
        }

        /* **while，不是 if。** 相簿每格影片 17ms 才回來一次，一次只補一塊
         * 一定跟不上；補到追上為止（單次最多補半個緩衝）。 */
        while (diff >= (int32_t)(USB_BUF_TOTAL / 2u)) {
            s_in += USB_BLOCK;
            if (s_in >= USB_BUF_TOTAL) { s_in = 0u; }
            if (!read_block(s_in)) { break; }
            diff -= (int32_t)USB_BLOCK;
        }
        UBOX[UB_POSMS] = usbaudio_wav_pos_ms();
    }
}



bool usbaudio_wav_active(void)
{
    return (s_state != US_IDLE) && (s_state != US_ENDED);
}

void usbaudio_wav_stop(void)
{
    UBOX[UB_STOPS]++;
    if (s_state != US_IDLE) {
        pump_stop();
        (void)USBH_AUDIO_Stop(&hUsbHostFS);
    }
    s_state = US_IDLE;
    if (s_open) { (void)f_close(&s_file); s_open = false; }
    s_paused = false;
    UBOX[UB_STREAM] = s_state;
}

void usbaudio_wav_pause(bool on)
{
    UBOX[UB_PAUSES]++;
    if (s_state != US_PLAYING) { return; }
    if (on == s_paused) { return; }
    s_paused = on;
    if (on) {
        (void)USBH_AUDIO_Suspend(&hUsbHostFS);
    } else {
        (void)USBH_AUDIO_Resume(&hUsbHostFS);
    }
}

uint32_t usbaudio_wav_len_ms(void)
{
    if (s_byterate == 0u) { return 0u; }
    return (uint32_t)(((uint64_t)s_hdr.data_len * 1000u) / s_byterate);
}

uint32_t usbaudio_wav_pos_ms(void)
{
    uint32_t bytes;

    if (s_byterate == 0u) { return 0u; }
    if (s_state < US_PLAYING) { return (uint32_t)(((uint64_t)s_org * 1000u) / s_byterate); }

    bytes = s_org + (class_sent() - s_sent0);
    if (bytes > s_hdr.data_len) { bytes = s_hdr.data_len; }
    return (uint32_t)(((uint64_t)bytes * 1000u) / s_byterate);
}

bool usbaudio_wav_seek_ms(uint32_t ms)
{
    uint32_t off;

    UBOX[UB_SEEKS]++;
    UBOX[UB_SEEKSTEP] = 1u;
    if (!s_open || s_byterate == 0u) { return false; }

    /* 對齊到取樣框（4 bytes）。**沒對齊的話左右聲道會對調** ——
     * ST 範例栽在這裡（偏移 34 不是 4 的倍數），症狀是刺耳的雜訊。 */
    off = (uint32_t)(((uint64_t)ms * s_byterate) / 1000u) & ~3u;
    if (off > s_hdr.data_len) { off = s_hdr.data_len & ~3u; }

    /* --- **不要在這裡停抽送。** ----------------------------------------
     * 開播序列住在 TIM7 的中斷裡；停掉抽送而忘了再啟動，序列就永遠不會
     * 重跑，狀態卡在 US_UNMUTE、active() 一直回 true，相簿等在那裡不動。
     * 第一版就是這樣：「滑到影片中間、之後暫停就當」。
     *
     * 抽送一直開著是安全的 —— 類別停播之後它就沒東西可送。
     * 順序也重要：**先把狀態放倒、填好緩衝，最後才讓序列重跑**，
     * 不然中斷可能在緩衝只填一半的時候就把它送出去。 */
    s_state = US_IDLE;
    (void)USBH_AUDIO_Stop(&hUsbHostFS);
    UBOX[UB_SEEKSTEP] = 2u;

    if (f_lseek(&s_file, s_hdr.data_off + off) != FR_OK) {
        UBOX[UB_STREAM] = s_state;
        return false;
    }

    UBOX[UB_SEEKSTEP] = 3u;
    s_left = s_hdr.data_len - off;
    s_org  = off;
    s_eof  = false;
    prefill();
    UBOX[UB_SEEKSTEP] = 4u;

    s_state = US_UNMUTE;                /* 填好了才讓序列重跑 */
    if (!s_pump_on) { pump_start(); }
    UBOX[UB_STREAM] = s_state;
    UBOX[UB_SEEKSTEP] = 5u;
    return true;
}

void usbaudio_set_volume(uint32_t pct)
{
    s_vol_pct = (uint8_t)((pct > 100u) ? 100u : pct);
    s_vol_dirty = true;
    /* **不要把狀態退回 US_VOL1。** 那樣會把整串開播序列重跑一遍，
     * 包含再呼叫一次 USBH_AUDIO_Play —— 播放中改音量不該碰開播流程。
     * 由 US_PLAYING 裡的 s_vol_dirty 處理。 */
}
