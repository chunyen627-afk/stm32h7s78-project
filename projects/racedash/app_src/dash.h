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

/* 儀表有幾種版面：點一下螢幕切到下一種、長按叫出選單（race_main.c）。 */
#define DASH_PAGES 10

/* 畫一整格儀表（800x480 橫向）。page 是第幾種版面，now_ms 用來做換檔燈閃爍。 */
void dash_draw(int page, const dsh1_t *d, uint32_t now_ms);

const char *dash_page_name(int page);

/* 剛切換時疊在畫面下方的「2/4 大字」。 */
void dash_draw_page_badge(int page);

/* 長按叫出的選單，current 會反白。dash_menu_hit 回傳點到第幾個，沒點到回 -1。 */
void dash_draw_menu(int current);
int  dash_menu_hit(int x, int y);

/* 沒有資料時的等待畫面。 */
void dash_draw_waiting(const dash_link_t *link, uint32_t now_ms);

#endif
