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

/* rate 直接給 Hz（8000~192000，卡上的 wav 是 48000）。刻意不用 BSP 的
 * AUDIO_FREQUENCY_* 常數 —— 那樣呼叫端就得引 BSP 標頭。
 * volume 是 0~100 的百分比。失敗回 false，回傳碼在 g_dbg_aud_init。 */
bool audio_init(uint32_t rate, uint32_t volume);

/* 播一段指定頻率的正弦波（循環）。第一步的驗證用。 */
bool audio_tone(uint32_t hz);

void audio_stop(void);

#endif /* AUDIO_OUT_H */
