/**
 * 隨身碟模式的切換。
 *
 * 外部 Flash 裡放兩個 app：
 *   0x70000000  相簿（bootloader 永遠跳這裡）
 *   0x71000000  USB 隨身碟（MSC_Standalone）
 *
 * 由**相簿的 main() 第一行**決定開哪一個：DTCM 0x20004000 有暗號就跳去
 * 隨身碟，否則照常跑相簿。相簿偵測到 VBUS 時留下暗號並重置。
 * bootloader 不參與判斷（它永遠跳 0x70000000）。
 */
#ifndef USBDRIVE_H
#define USBDRIVE_H

#include <stdbool.h>

/* 偵測到 USB 插入時呼叫：留暗號然後重置。永不返回。
 * 暗號放 DTCM —— 放 AXI SRAM 會被 bootloader 的堆疊踩掉，
 * 症狀是「時好時壞」，原因見 usbdrive.c 的說明。 */
void usbdrive_request_switch(void);

#endif /* USBDRIVE_H */
