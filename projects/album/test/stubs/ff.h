/* PC 測試台用的 FatFs 替身。測試台不讀 SD 卡，影像直接餵進 RGB 緩衝區。 */
#ifndef HOST_STUB_FF_H
#define HOST_STUB_FF_H

#include <stdint.h>

#define FR_OK    0
#define FA_READ  1

typedef unsigned int UINT;
typedef int          FRESULT;
typedef struct { uint32_t dummy; } FIL;

static inline FRESULT f_open(FIL *f, const char *p, uint8_t m)
{
    (void)f; (void)p; (void)m; return FR_OK;
}
static inline uint32_t f_size(FIL *f) { (void)f; return 0u; }
static inline FRESULT f_read(FIL *f, void *b, UINT n, UINT *got)
{
    (void)f; (void)b; (void)n; *got = 0u; return FR_OK;
}
static inline FRESULT f_close(FIL *f) { (void)f; return FR_OK; }

#endif /* HOST_STUB_FF_H */
