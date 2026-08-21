#ifndef PHOTO_H_INCLUDED
#define PHOTO_H_INCLUDED

#include <stdbool.h>

typedef enum {
    PHOTO_OK = 0,
    PHOTO_ERR_READ,      /* 檔案開不了或讀不完 */
    PHOTO_ERR_DECODE,    /* 不是有效的 JPEG，或解碼器回報錯誤 */
    PHOTO_ERR_TOO_BIG,   /* 解出來超過緩衝區 */
    PHOTO_ABORTED,       /* 中途被要求放棄（使用者按了暫停）*/
} photo_result_t;

/* 註冊「要不要放棄」的檢查函式。
 *
 * 解一張照片要 1.5 秒，期間使用者按暫停的話，正在解的這張其實不需要了 ——
 * 他要停的是螢幕上那張。沒有這個機制的話，按下去得等解碼跑完才有反應。
 * photo_show() 會在管線各階段之間、以及兩個大迴圈裡每隔幾十列檢查一次，
 * 回傳 true 就立刻放棄並回 PHOTO_ABORTED。 */
void photo_set_abort_check(bool (*fn)(void));

bool           photo_init(void);
/* 解碼一張照片並畫進目前的 back buffer。不會自己 present。 */
photo_result_t photo_show(const char *path);

#endif /* PHOTO_H_INCLUDED */
