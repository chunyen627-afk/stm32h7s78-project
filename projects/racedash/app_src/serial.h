#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/* UART4 = ST-LINK 的虛擬 COM 埠（板子插 CN7 那條線，電腦上是 COM3 之類）。
 * 電腦上的 RaceDash Bridge 從這裡每秒送 60 行遙測（DSH1）。 */
bool serial_init(uint32_t baud);

/* 收到完整一行就回傳長度，內容在 buf（不含換行、以 NUL 結尾）；還沒收完回 -1。
 * 收到一半的行會先累積在 buf 裡，所以**每次都要傳同一塊 buf**。
 * 超過 cap-1 的行整行丟掉，不回傳半截。 */
int serial_read_line(char *buf, int cap);

/* 診斷計數：收到的位元組、溢位（中斷沒來得及收）、整行太長被丟掉的次數。 */
extern volatile uint32_t g_serial_rx_bytes;
extern volatile uint32_t g_serial_rx_overrun;
extern volatile uint32_t g_serial_line_dropped;

#endif
