/**
 * WAV 檔頭解析 —— I2S（audio_out.c）與 USB（usbaudio.c）共用。
 *
 * 為什麼抽出來而不是各留一份：這是**純邏輯**，沒有硬體也沒有時序，
 * 抽出來的風險很低；而複製一份的代價是未來遇到某種 wav 的怪癖時
 * 只會修到一邊。ST 的範例就是栽在檔頭上（寫死第 44 個位元組，
 * 遇到 ffmpeg 插的 LIST 區塊就整個錯開）——
 * 這種坑不該有兩個地方各踩一次。
 */
#ifndef WAV_HDR_H
#define WAV_HDR_H

#include <stdbool.h>
#include <stdint.h>
#include "ff.h"

typedef struct {
    uint32_t rate;       /* 取樣率 */
    uint16_t channels;
    uint16_t bits;
    uint32_t data_off;   /* data 區塊在檔案裡的起始位移 */
    uint32_t data_len;   /* data 區塊的長度 */
} wav_hdr_t;

/* 走區塊鏈找 fmt 與 data。**不要假設 data 在第 44 個位元組** ——
 * ffmpeg 會在 fmt 與 data 之間插 LIST 區塊，實測我們的檔案 data 在第 78 個。
 *
 * 成功時檔案指標正好停在資料開頭。失敗回 false（壞檔、不是 WAV、
 * 或區塊鏈繞了 32 圈還沒找到 data）。 */
bool wav_hdr_parse(FIL *fp, wav_hdr_t *out);

#endif /* WAV_HDR_H */
