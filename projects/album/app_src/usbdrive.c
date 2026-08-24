/* 狀態：**可用**（2026-08-24 實測）。插上 USB1 自動變隨身碟、拔線自動回相簿。
 *
 * 這個檔案只負責「留暗號 + 重置」。真正的跳轉在相簿的 main() 第一行
 * （cube 的 Templates/Album/Appli/Src/main.c，由 patch_project.py 植入）。
 *
 * 讓這件事成立的兩個根因，都不在這個檔案裡，找了很久：
 *
 * 1. **HSE**。MSC_Standalone 沒有自己的 SystemClock_Config，時脈全靠
 *    bootloader；它原廠的走 HSE，相簿的只開 HSI —— 而 USB HS 的 PHY 需要
 *    RCC_USBPHYCCLKSOURCE_HSE。已在相簿 bootloader 補開 HSE，PLL 來源
 *    仍是 HSI，所以相簿的時序完全不變。
 *
 * 2. **快取一致性**。usbd_storage_if.c 的 SD 讀寫沒有快取維護。症狀是磁碟
 *    列舉正常、容量正確、MBR 也讀得出來（第一次讀時快取是冷的），
 *    但檔案系統掛不上。讀後 InvalidateDCache、寫前 CleanDCache 就好了。
 */
#include "usbdrive.h"
#include "vbus.h"
#include "main.h"

/* 跟相簿 main() 約定的暗號，放 DTCM。
 *
 * **不能放 AXI SRAM。** 原本放 0x24071BF0，那裡在 bootloader 堆疊頂端
 * （SP = 0x24072000）下面只有 1040 bytes —— bootloader 跑外部記憶體初始化
 * 就會把它踩掉。症狀是「有時候回得來、有時候回不來」，查了整晚。
 * 實測：寫 0xA5A5A5A5 進去、重置，讀回來變成別的值。
 *
 * DTCM 是緊耦合記憶體，bootloader 完全不碰（它的堆疊在 AXI SRAM），
 * 實測寫進去重置後原封不動。**而且 DTCM 不經過快取**，所以也不再需要
 * SCB_CleanDCache_by_Addr —— 今天那個「寫入卡在快取被 InvalidateDCache
 * 丟掉」的整類問題一併消失。 */
#define USBD_FLAG_ADDR   ((volatile uint32_t *)0x20004000u)
#define USBD_FLAG_MAGIC  0x55534244u   /* "USBD" */

volatile uint32_t g_usbd_switch_req;   /* 除錯：有沒有要求過切換 */

/* 為什麼不是相簿自己跳過去
 * --------------------------
 * 一開始的作法是相簿讀到 VBUS 就直接跳 0x71000000，完全不動 bootloader。
 * 跳轉本身是成功的（VTOR 讀出來就是新位址、uwTick 照跳代表中斷也正常），
 * **但 USB 就是不列舉**。
 *
 * 原因是隨身碟 app 假設自己從重置開始跑，而相簿在跳之前已經設定過系統時脈、
 * MPU、快取與一堆周邊。USB HS 的 PHY 要 RCC_PERIPHCLK_USBPHYC 從 HSE 取時脈，
 * PLL 已經在跑的情況下重跑 SystemClock_Config() 很可能提早返回，PHY 的時脈
 * 就沒設起來。
 *
 * 這是**一整類**的污染問題（ADC2 被佔用只是其中一個，修掉也沒用），
 * 一個一個追不會有盡頭。所以改成：相簿只留暗號然後重置，由 bootloader 在
 * 完全乾淨的狀態下跳過去 —— 隨身碟 app 因此跟「單獨燒進去開機」時
 * 一模一樣，那個情況是實測會動的。
 */

void usbdrive_request_switch(void)
{
    *USBD_FLAG_ADDR   = USBD_FLAG_MAGIC;
    g_usbd_switch_req = 1u;

    /* DTCM 不經過快取，寫進去就是寫進去了 —— 不必 SCB_CleanDCache_by_Addr。
     * 放 AXI SRAM 的時候還得防「寫入卡在快取、被隨身碟 app 的
     * SCB_InvalidateDCache() 丟掉」，換到 DTCM 之後那整類問題就消失了。 */
    __DSB();

    NVIC_SystemReset();
}
