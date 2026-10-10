#ifndef DASH_H
#define DASH_H

#include <stdbool.h>
#include <stdint.h>
#include "dsh1.h"

/* 連線狀態，等待畫面用來告訴使用者卡在哪一步。 */
typedef struct {
    uint32_t rx_bytes;      /* 序列埠總共收到幾個位元組 */
    uint32_t lines_ok;      /* 解析成功的行數 */
    uint32_t lines_bad;     /* 格式不對的行數 */
} dash_link_t;

/* 畫一整格儀表（800x480 橫向）。now_ms 用來做換檔燈閃爍。 */
void dash_draw(const dsh1_t *d, uint32_t now_ms);

/* 沒有資料時的等待畫面。 */
void dash_draw_waiting(const dash_link_t *link, uint32_t now_ms);

#endif
