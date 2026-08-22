/* PC 測試台用的 jpeg_utils 替身。轉色不在測試範圍內（測試台直接餵 RGB888），
 * 這裡只要讓 photo.c 編得過。 */
#ifndef HOST_STUB_JPEG_UTILS_H
#define HOST_STUB_JPEG_UTILS_H

#include <stdint.h>
#include "main.h"

#define JPEG_444_SUBSAMPLING  0
#define JPEG_420_SUBSAMPLING  1
#define JPEG_422_SUBSAMPLING  2

typedef uint32_t (*JPEG_YCbCrToRGB_Convert_Function)(uint8_t *pInBuffer,
                                                     uint8_t *pOutBuffer,
                                                     uint32_t BlockIndex,
                                                     uint32_t DataCount,
                                                     uint32_t *ConvertedDataCount);

static inline void JPEG_InitColorTables(void) { }

static inline int JPEG_GetDecodeColorConvertFunc(JPEG_ConfTypeDef *info,
                                                 JPEG_YCbCrToRGB_Convert_Function *f,
                                                 uint32_t *nb_mcu)
{
    (void)info; (void)f; *nb_mcu = 0u; return HAL_OK;
}

#endif /* HOST_STUB_JPEG_UTILS_H */
