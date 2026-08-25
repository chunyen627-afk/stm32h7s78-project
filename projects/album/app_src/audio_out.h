/**
 * 音訊輸出（WM8904 / I2S6 / 耳機孔 CN16）。
 *
 * 目前只有「能不能出聲」這一層。串流 WAV、與影片同步都還沒做 ——
 * 刻意的：先單獨驗證最底層那件事，再往上加（見 audio.c 的說明）。
 */
/* **檔名不能叫 audio.h。** BSP 的 Components/Common 底下就有一個 audio.h
 * （定義 AUDIO_Drv_t），而 stm32h7s78_discovery_audio.h 會 #include "audio.h"
 * —— 我們的 include 路徑排在前面，它就會抓到這一份，然後整個 BSP 編不過。 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdbool.h>
#include <stdint.h>

/* 診斷變數（板子沒有 UART，一律 SWD 讀）。
 *
 * **0xFFFFFFFF = 這段程式根本沒執行。** 這個哨兵值不是裝飾：
 * BSP_ERROR_NONE 就是 0，而全域變數的初值也是 0 —— 讀到 0 的時候
 * 分不出「初始化成功」還是「壓根沒跑到」。第一版就卡在這個歧義上，
 * 花掉一整輪才發現真相是後者（相簿停在選單，而選單迴圈當時根本沒有
 * 檢查 g_dbg_audiotest）。
 *
 * 附帶好處：初值非 0 的全域會進 .data，由啟動碼從外部 Flash 複製過來，
 * 所以「讀到 0xFFFFFFFF」同時也證明了 .data 複製是好的。 */
#define AUDIO_UNSET   0xFFFFFFFFu

extern volatile uint32_t g_dbg_aud_init;   /* BSP_AUDIO_OUT_Init 的回傳值 */
extern volatile uint32_t g_dbg_aud_play;   /* BSP_AUDIO_OUT_Play 的回傳值 */
extern volatile uint32_t g_dbg_aud_step;   /* 走到哪裡（位元圖，見下） */
extern volatile uint32_t g_dbg_aud_pll;    /* 音訊 PLL3 設定的回傳值 */
extern volatile uint32_t g_dbg_aud_sel;    /* SPI6 選時脈來源的回傳值 */
extern volatile uint32_t g_dbg_aud_half;   /* 半滿回呼進來幾次 */
extern volatile uint32_t g_dbg_aud_full;   /* 全滿回呼進來幾次 */
extern volatile uint32_t g_dbg_aud_err;    /* 錯誤回呼進來幾次 */

/* g_dbg_aud_step 的位元。**一路 OR 上去，不是覆寫成最後一個狀態** ——
 * 「最後一次的值」是最會騙人的診斷資料（board-notes 18.2）。
 * 讀一次就知道流程停在哪一步，尤其分得出「BSP 回了錯誤碼」與
 * 「BSP 進去就沒再出來」這兩件完全不同的事。 */
#define AUD_STEP_TRIGGER    (1u << 0)   /* 主迴圈看到 g_dbg_audiotest */
#define AUD_STEP_INIT_CALL  (1u << 1)   /* 進到 audio_init() */
#define AUD_STEP_BSP_RET    (1u << 2)   /* BSP_AUDIO_OUT_Init 回來了 */
#define AUD_STEP_INIT_OK    (1u << 3)   /* 初始化回報成功 */
#define AUD_STEP_TONE_CALL  (1u << 4)   /* 進到 audio_tone() */
#define AUD_STEP_TONE_FILL  (1u << 5)   /* 波形算完、快取也清了 */
#define AUD_STEP_PLAY_RET   (1u << 6)   /* BSP_AUDIO_OUT_Play 回來了 */
/* 時脈設定那一段（在 BSP_AUDIO_OUT_Init 內部被呼叫，而且是在 WM8904 探測
 * **之後**）。所以 CLK_CALL 有亮就代表 I2C 那一關過了。 */
#define AUD_STEP_CLK_CALL   (1u << 7)   /* 進到 MX_I2S6_ClockConfig */
#define AUD_STEP_CLK_OSC    (1u << 8)   /* HAL_RCC_OscConfig 回來了 */
#define AUD_STEP_CLK_SEL    (1u << 9)   /* HAL_RCCEx_PeriphCLKConfig 回來了 */

/* rate 直接給 Hz（8000~192000，卡上的 wav 是 48000）。刻意不用 BSP 的
 * AUDIO_FREQUENCY_* 常數 —— 那樣呼叫端就得引 BSP 標頭。
 * volume 是 0~100 的百分比。失敗回 false，回傳碼在 g_dbg_aud_init。 */
bool audio_init(uint32_t rate, uint32_t volume);

/* 播一段指定頻率的正弦波（循環）。第一步的驗證用。 */
bool audio_tone(uint32_t hz);

void audio_stop(void);

#endif /* AUDIO_OUT_H */
