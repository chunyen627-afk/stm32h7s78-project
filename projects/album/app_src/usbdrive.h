/**
 * 隨身碟模式的切換。
 *
 * 外部 Flash 裡放兩個 app：
 *   0x70000000  相簿（bootloader 永遠跳這裡）
 *   0x71000000  USB 隨身碟（MSC_Standalone）
 *
 * 由 bootloader 決定開哪一個：BKPSRAM 裡有暗號就開隨身碟，否則開相簿。
 * 相簿偵測到 USB 插上時留下暗號並重置。
 */
#ifndef USBDRIVE_H
#define USBDRIVE_H

#include <stdbool.h>

/* 偵測到 USB 插入時呼叫：留暗號給 bootloader，然後重置。永不返回。
 * 由 bootloader 在乾淨狀態下跳到隨身碟 app —— 相簿自己跳過去的話
 * USB 不會列舉，原因見 usbdrive.c 的說明。 */
void usbdrive_request_switch(void);

#endif /* USBDRIVE_H */
