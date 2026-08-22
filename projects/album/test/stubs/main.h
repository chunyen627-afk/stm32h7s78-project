/* PC 測試台用的 HAL 替身。
 *
 * photo.c 的影像管線（plan_geometry / downscale / sharpen_dither_rotate）
 * 是純運算，不碰任何硬體；但同一個檔案裡的 photo_show() 會用到 HAL、FatFs
 * 與 JPEG 硬體。為了「直接編譯 photo.c 本體」而不是重寫一份演算法，
 * 這裡把那些相依補成能編過的空殼 —— 測試台不會呼叫它們。
 */
#ifndef HOST_STUB_MAIN_H
#define HOST_STUB_MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define HAL_OK 0
typedef int HAL_StatusTypeDef;

/* --- 快取維護：主機上沒有 D-Cache，全部是 no-op --- */
static inline void SCB_InvalidateDCache_by_Addr(uint32_t *a, int32_t n)
{
    (void)a; (void)n;
}

/* --- DWT 週期計數器 --- */
typedef struct { uint32_t DEMCR; } CoreDebug_Type_Stub;
typedef struct { uint32_t CYCCNT; uint32_t CTRL; } DWT_Type_Stub;
extern CoreDebug_Type_Stub *CoreDebug;
extern DWT_Type_Stub       *DWT;
extern uint32_t             SystemCoreClock;
#define CoreDebug_DEMCR_TRCENA_Msk   (1u << 24)
#define DWT_CTRL_CYCCNTENA_Msk       (1u << 0)

static inline uint32_t HAL_GetTick(void) { return 0u; }

/* --- JPEG 硬體 --- */
#define JPEG ((void *)0)
#define __HAL_RCC_JPEG_CLK_ENABLE() do { } while (0)

typedef struct { void *Instance; } JPEG_HandleTypeDef;
typedef struct {
    uint32_t ImageWidth;
    uint32_t ImageHeight;
    uint32_t ChromaSubsampling;
} JPEG_ConfTypeDef;

static inline int HAL_JPEG_Init(JPEG_HandleTypeDef *h) { (void)h; return HAL_OK; }
static inline int HAL_JPEG_Decode(JPEG_HandleTypeDef *h, uint8_t *in, uint32_t inl,
                                  uint8_t *out, uint32_t outl, uint32_t to)
{
    (void)h; (void)in; (void)inl; (void)out; (void)outl; (void)to;
    return HAL_OK;
}
static inline int HAL_JPEG_GetInfo(JPEG_HandleTypeDef *h, JPEG_ConfTypeDef *c)
{
    (void)h; (void)c; return HAL_OK;
}
static inline int HAL_JPEG_ConfigInputBuffer(JPEG_HandleTypeDef *h,
                                             uint8_t *p, uint32_t n)
{
    (void)h; (void)p; (void)n; return HAL_OK;
}

#endif /* HOST_STUB_MAIN_H */
