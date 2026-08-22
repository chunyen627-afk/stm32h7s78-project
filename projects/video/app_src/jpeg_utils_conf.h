/**
 * jpeg_utils 的設定：影片版直接輸出 RGB565。
 *
 * 跟相簿的差別在於後面還要不要處理：
 *
 *   相簿  解成 RGB888 -> 縮放 -> 銳化 -> 抖動 -> 量化成 RGB565
 *         中間要保留 8 位元精度，一開始就砍成 5/6/5 的話誤差會累積。
 *
 *   影片  影格已經是面板尺寸，不縮放、不後製，轉色的結果就是最終畫面。
 *         所以直接輸出 RGB565 寫進 framebuffer —— 少一個中間緩衝區、
 *         每像素少寫一個 byte，而且省掉一整趟 PSRAM 來回。
 *
 * 注意 jpeg_utils 的 RGB565 路徑是用 __IO（volatile）指標寫輸出的，
 * 編譯器沒辦法最佳化那些存取；RGB888 路徑則不是。所以「少寫一個 byte」
 * 未必真的比較快，這點要實測（board-notes 16.1 那個 4.8 倍的教訓：
 * 不要假設哪條路徑比較快）。
 */
#ifndef __JPEG_UTILS_CONF_H__
#define __JPEG_UTILS_CONF_H__

#include "stm32h7rsxx_hal.h"

#define JPEG_ARGB8888        0
#define JPEG_RGB888          1
#define JPEG_RGB565          2

#define USE_JPEG_DECODER     1
#define USE_JPEG_ENCODER     0

#define JPEG_RGB_FORMAT      JPEG_RGB565

/* RGB565 是整個 16 位元值一次寫入，沒有位元組順序的問題 —— 相簿那個
 * 「RED_OFFSET 是位元位移不是位元組索引」的坑只發生在 RGB888
 * （board-notes 11.6）。 */
#define JPEG_SWAP_RB         0

#endif /* __JPEG_UTILS_CONF_H__ */
