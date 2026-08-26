/**
 * USB 無線耳機（USB Host / UAC1）——— 第一階段：讓它活起來。
 *
 * 說明見 usbaudio.h。底層的 LL 膠合在 usbh_glue.c。
 */
#include "usbaudio.h"

#include "usbh_core.h"
#include "usbh_audio.h"

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
#define UB_INIT_STARTOK (1u << 3)

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

/* 應用層看到的狀態。**volatile**：TIM7 的中斷之後也會改到相關狀態。 */
static volatile uint8_t s_appli = 0u;   /* 0 閒置 1 已連接 2 就緒 3 斷線 */
static volatile uint8_t s_inited = 0u;

static void usb_user_process(USBH_HandleTypeDef *phost, uint8_t id)
{
    switch (id) {
    case HOST_USER_DISCONNECTION:
        s_appli = 3u;
        break;

    case HOST_USER_CLASS_ACTIVE:
        s_appli = 2u;
        UBOX[UB_VIDPID] = ((uint32_t)phost->device.DevDesc.idVendor << 16) |
                          phost->device.DevDesc.idProduct;
        {
            AUDIO_HandleTypeDef *h =
                (AUDIO_HandleTypeDef *)phost->pActiveClass->pData;

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

void usbaudio_init(void)
{
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

    if (USBH_Init(&hUsbHostFS, usb_user_process, HOST_FS) != USBH_OK) {
        return;
    }
    UBOX[UB_INIT] |= UB_INIT_HOSTOK;

    if (USBH_RegisterClass(&hUsbHostFS, USBH_AUDIO_CLASS) != USBH_OK) {
        return;
    }
    UBOX[UB_INIT] |= UB_INIT_CLASSOK;

    if (USBH_Start(&hUsbHostFS) != USBH_OK) {
        return;
    }
    UBOX[UB_INIT] |= UB_INIT_STARTOK;

    s_inited = 1u;
}

void usbaudio_process(void)
{
    if (s_inited == 0u) {
        return;
    }

    UBOX[UB_PROC]++;
    UBOX[UB_GSTATE] = (uint32_t)hUsbHostFS.gState;

    (void)USBH_Process(&hUsbHostFS);
}

bool usbaudio_ready(void)
{
    return (s_appli == 2u);
}
