/* jpeg_utils 的設定。相簿只解碼、輸出 RGB565（面板的原生格式）。 */
#ifndef __JPEG_UTILS_CONF_H__
#define __JPEG_UTILS_CONF_H__

#include "stm32h7rsxx_hal.h"

#define JPEG_ARGB8888        0
#define JPEG_RGB888          1
#define JPEG_RGB565          2

#define USE_JPEG_DECODER     1
#define USE_JPEG_ENCODER     0

/* 解碼成 RGB888 而不是面板的原生 RGB565。
 *
 * 多花一個 byte 的記憶體，換來整條後製都在 8 位元精度下進行：縮放平均不會
 * 因為 5/6/5 的量化誤差累積，銳化也有足夠的動態範圍。最後才一次量化到
 * RGB565 並加上抖動，漸層才不會出現色帶。 */
#define JPEG_RGB_FORMAT      JPEG_RGB888
/* 位元組順序。
 *
 * jpeg_utils 的 RED/GREEN/BLUE_OFFSET 是「24 位元值裡的位元位移」，寫進記憶體
 * 時是 pOutAddr + OFFSET/8。SWAP_RB=0 時 RED_OFFSET=16，也就是紅色落在第 3 個
 * 位元組 —— 記憶體順序是 B,G,R。設成 1 讓 RED_OFFSET=0，順序才是直覺的 R,G,B，
 * 後面的縮放與銳化程式碼才不用到處記得紅藍是反的。 */
#define JPEG_SWAP_RB         1

#endif /* __JPEG_UTILS_CONF_H__ */
