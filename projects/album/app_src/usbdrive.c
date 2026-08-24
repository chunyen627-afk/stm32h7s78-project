/* 狀態：**尚未完成，刻意不編進韌體**（patch_project.py 沒有登記這個檔案）。
 *
 * 保留下來是為了裡面記的那些踩坑過程 —— 相簿自己跳過去為什麼不行、
 * 從 bootloader 跳又碰到什麼。要接手這個功能的人先讀完這裡，
 * 可以省下我今天花掉的那些燒錄循環。
 *
 * 真正的根因見 docs 與 boot 的 SystemClock_Config：隨身碟 app 沒有自己的
 * 時脈設定，而相簿的 bootloader 只開 HSI，USB HS 的 PHY 需要 HSE。
 * 補開 HSE 之後 RCC->CR 的 HSEON/HSERDY 確實亮了，但仍未列舉 ——
 * 兩顆 bootloader 的 PLL 樹整組不同（HSI 對 HSE），還有別的差異沒查完。
 */
#include "usbdrive.h"
#include "vbus.h"
#include "main.h"

/* 跟 bootloader 約定的暗號，放 AXI SRAM 的頂端。
 *
 * 原本放 BKPSRAM(0x38800000)，但那在這顆晶片上光開時脈不夠 —— 備援網域
 * 還要解除寫入保護，直接存取會讓 bootloader 當在那裡，連相簿都開不起來
 * （VTOR 停在 0x08000000）。AXI SRAM 不需要任何設定，熱重置後內容照樣
 * 留著，而相簿的 .bss 只到約 0x24042000，離這裡很遠。 */
#define USBD_FLAG_ADDR   ((volatile uint32_t *)0x24071BF0u)
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

    /* **一定要把快取寫回去。** 相簿的 D-Cache 是開的，AXI SRAM 又是可快取的，
     * 所以上面那行很可能只寫進快取。`__DSB()` 只保證指令順序，**不會**把快取
     * 內容推到記憶體 —— 重置後 bootloader 從 SRAM 讀到的會是舊值，暗號等於
     * 沒留。這種 bug 的症狀是「偶爾才切換成功」，最難查。 */
    SCB_CleanDCache_by_Addr((uint32_t *)USBD_FLAG_ADDR, 32);
    __DSB();

    NVIC_SystemReset();
}
