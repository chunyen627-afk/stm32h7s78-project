/**
 * 音訊輸出：WM8904，走 I2S6（SPI6 的 I2S 模式）+ GPDMA1 Channel 2。
 * 實體輸出只有 3.5mm 耳機孔 CN16，板子上沒有喇叭功放。
 *
 * **這是第一步：只證明「能不能發出聲音」。**
 * 從 RAM 播一段自己產生的正弦波，不碰 SD、不解析 WAV、不做同步。
 * 有聲音就代表 BSP + I2S6 + WM8904 這條鏈路是通的；沒有的話也知道問題
 * 一定在這一層，不必去懷疑檔案或時序。
 *
 * 這個順序是有代價換來的：隨身碟那輪把三個沒單獨驗證過的東西疊在一起才開始
 * 測，每次失敗都分不清是哪一個造成的（board-notes 22 與第八章第 6 條）。
 *
 * 已知的風險，之後要量：JPEG 解碼目前是**輪詢式**的，而音訊 DMA 會持續產生
 * 中斷去打斷它。這不是理論顧慮 —— repo 裡有一個 2026-08-22 的 stash，
 * 訊息就寫著「中斷干擾輪詢解碼」。所以第二步一定要量格率，不能只聽聲音。
 */
#include "audio_out.h"
#include "main.h"
#include "stm32h7s78_discovery_audio.h"

#include <math.h>

/* BSP_AUDIO_OUT_Play 的 NbrOfBytes 是 uint16 換算來的，**上限 65535**。
 * 取 32KB：48kHz 立體聲 16-bit 下等於 170ms，夠長到補資料來得及，
 * 又不佔太多 AXI SRAM（相簿已經用掉 264KB / 465KB）。 */
#define AUDIO_BUF_BYTES   (32u * 1024u)

static int16_t  g_buf[AUDIO_BUF_BYTES / 2u];   /* 交錯的 L,R,L,R... */
static bool     g_ready;
static uint32_t g_rate;

/* 診斷用，SWD 讀得到。板子沒有 UART，這是唯一的觀察方式。 */
volatile uint32_t g_dbg_aud_init;     /* BSP_AUDIO_OUT_Init 的回傳值 */
volatile uint32_t g_dbg_aud_play;     /* BSP_AUDIO_OUT_Play 的回傳值 */
volatile uint32_t g_dbg_aud_half;     /* 半滿回呼進來幾次 */
volatile uint32_t g_dbg_aud_full;     /* 全滿回呼進來幾次 */
volatile uint32_t g_dbg_aud_err;      /* 錯誤回呼進來幾次 */

bool audio_init(uint32_t rate, uint32_t volume)
{
    BSP_AUDIO_Init_t cfg;
    int32_t          r;

    if (g_ready) { return true; }

    cfg.Device        = AUDIO_OUT_HEADPHONE;
    cfg.SampleRate    = rate;
    cfg.BitsPerSample = AUDIO_RESOLUTION_16B;
    cfg.ChannelsNbr   = 2;
    cfg.Volume        = volume;

    r = BSP_AUDIO_OUT_Init(0, &cfg);
    g_dbg_aud_init = (uint32_t)r;
    if (r != BSP_ERROR_NONE) { return false; }

    g_rate  = rate;
    g_ready = true;
    return true;
}

bool audio_tone(uint32_t hz)
{
    uint32_t frames = AUDIO_BUF_BYTES / 4u;   /* 一個 frame = L+R = 4 bytes */
    int32_t  r;

    if (!g_ready) { return false; }

    /* **讓緩衝首尾相接**：取整數個週期，否則循環播放時每一圈的接縫都會
     * 「啪」一聲。frames 取到最接近的整數週期倍數。 */
    {
        uint32_t period = (hz > 0u) ? (g_rate / hz) : frames;
        if (period > 0u && period < frames) {
            frames = (frames / period) * period;
        }
    }

    for (uint32_t i = 0; i < frames; i++) {
        /* 振幅只給三分之一。第一次出聲最怕的是音量開太大 —— 耳機貼著耳朵
         * 的話會很傷，而這階段還不確定音量設定有沒有生效。 */
        double   t = (2.0 * 3.14159265358979 * (double)hz * (double)i) / (double)g_rate;
        int16_t  s = (int16_t)(10000.0 * sin(t));

        g_buf[i * 2u]      = s;   /* L */
        g_buf[i * 2u + 1u] = s;   /* R */
    }

    /* **DMA 從記憶體讀，資料還在 D-Cache 裡的話它讀到的是舊內容。**
     * 這一輪已經在 USB 那邊踩過同一類問題兩次（board-notes 22.3 / 22.5）。 */
    SCB_CleanDCache_by_Addr((uint32_t *)g_buf, (int32_t)(frames * 4u));

    r = BSP_AUDIO_OUT_Play(0, (uint8_t *)g_buf, frames * 4u);
    g_dbg_aud_play = (uint32_t)r;
    return r == BSP_ERROR_NONE;
}

void audio_stop(void)
{
    if (g_ready) {
        (void)BSP_AUDIO_OUT_Stop(0);
    }
}

/* BSP 的回呼。第一步只計次 —— 有沒有進來就知道 DMA 有沒有在跑，
 * 這比「聽起來好像有聲音」可靠。 */
void BSP_AUDIO_OUT_HalfTransfer_CallBack(uint32_t Instance)
{
    (void)Instance;
    g_dbg_aud_half++;
}

void BSP_AUDIO_OUT_TransferComplete_CallBack(uint32_t Instance)
{
    (void)Instance;
    g_dbg_aud_full++;
}

void BSP_AUDIO_OUT_Error_CallBack(uint32_t Instance)
{
    (void)Instance;
    g_dbg_aud_err++;
}
