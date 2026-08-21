#ifndef PHOTO_H_INCLUDED
#define PHOTO_H_INCLUDED

#include <stdbool.h>

typedef enum {
    PHOTO_OK = 0,
    PHOTO_ERR_READ,      /* 檔案開不了或讀不完 */
    PHOTO_ERR_DECODE,    /* 不是有效的 JPEG，或解碼器回報錯誤 */
    PHOTO_ERR_TOO_BIG,   /* 解出來超過緩衝區 */
} photo_result_t;

bool           photo_init(void);
/* 解碼一張照片並畫進目前的 back buffer。不會自己 present。 */
photo_result_t photo_show(const char *path);

#endif /* PHOTO_H_INCLUDED */
