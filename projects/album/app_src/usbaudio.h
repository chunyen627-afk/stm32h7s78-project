/**
 * USB 無線耳機（USB Host / UAC1，走 CN17 = USB2 全速）。
 *
 * 這是把治具（projects/usbaudio）的成果接進相簿的第一階段：
 * **只負責讓 USB 主機跑起來、列舉到 dongle、把類別掛上**。
 * 音訊資料還沒接 —— audio_out.c 完全沒有動。
 *
 * 為什麼分階段：整合一次要動建置系統、記憶體、時脈、中斷、音訊路徑，
 * 五件事一起上出問題就分不出是誰。先把「USB 活得起來而且不影響相簿」
 * 這一件驗掉（board-notes 八：一次只加一個沒驗證過的東西）。
 *
 * 硬體：dongle 插 **CN17（USB2）**，JP1 要同時插 STLK + USB2 兩個跳線帽。
 * **這樣接的時候 CN17 不能再插電腦或充電器**（兩個 5V 對接）。
 * 相簿的隨身碟模式用 CN18（USB1 / HS），是另一顆控制器，不衝突。
 */
#ifndef USBAUDIO_H
#define USBAUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* 開機時呼叫一次。失敗不影響相簿其他功能 —— 沒有 dongle 是常態。 */
void usbaudio_init(void);

/* **從 nap() 裡呼叫。** 相簿所有的迴圈都經過 nap()，所以這一個插入點
 * 就涵蓋了選單、看照片、播影片全部的迴圈，不必一個一個改。
 *
 * 列舉與類別請求階段由這裡驅動；資料串流階段會交給 TIM7 的中斷
 * （USB 的等時端點要 1000 包/秒，而相簿每格影片要 17ms —— 主迴圈餵不動，
 * 見 projects/usbaudio/README.md 的實測）。 */
void usbaudio_process(void);

/* dongle 在線而且 UAC1 類別已經掛好，可以開始送音訊。 */
bool usbaudio_ready(void);

#endif /* USBAUDIO_H */
