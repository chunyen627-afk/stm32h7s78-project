#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把 ST 的 MSC_Standalone 範例改成「相簿的隨身碟模式」能用的樣子。

範例本身就是對的起點：儲存後端直接是 SD 卡（BSP_SD_*Blocks_DMA），不是內建
Flash 的假磁碟。這裡只做四件必要的修改，每一件都是實測踩出來的。

冪等：每個 replace 的樣式套用後就不再匹配，重跑不會重複植入。
"""
import io
import os
import sys

# tools/ -> usbdrive/ -> projects/ -> repo 根目錄，四層。
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", ".."))
CUBE = os.environ.get("CUBE_DIR", os.path.join(REPO, "cube"))
PROJ = os.path.join(CUBE, "Projects", "STM32H7S78-DK", "Applications",
                    "USB_Device", "MSC_Standalone")


def _edit(path, pairs, label, marker=None):
    """對 path 套用 (舊, 新) 的替換。

    marker：一段「套用過就一定會出現」的短字串。用它判斷是否已改過，
    比拿整段含註解的新字串去比對可靠得多 —— 註解只要動一個字就對不上，
    然後會誤報「找不到樣式」。
    """
    if not os.path.isfile(path):
        print("  !! 找不到 %s" % path)
        return False

    s = io.open(path, encoding="utf-8", errors="surrogateescape").read()

    if marker is not None and marker in s:
        print("  %s 已改過" % label)
        return False

    before = s
    for old, new in pairs:
        if old not in s:
            print("  !! %s：找不到樣式，沒有套用 -> %s" % (label, old.strip()[:50]))
        s = s.replace(old, new)

    if s == before:
        print("  %s 已改過" % label)
        return False

    io.open(path, "w", encoding="utf-8", errors="surrogateescape",
            newline="").write(s)
    print("  %s 已修改" % label)
    return True


def patch_link_address():
    """搬到 0x71000000。0x70000000 是相簿，兩個 app 並存在同一顆外部 NOR。

    NOR 有 128MB，相簿 1.5MB + 隨身碟 2.4MB，隔 16MB 綽綽有餘而且位址好記。
    """
    path = os.path.join(PROJ, "STM32CubeIDE", "Appli",
                        "STM32H7S7L8HXH_RAMxspi1_ROMxspi2_app.ld")
    _edit(path, [
        ("__FLASH_BEGIN  = 0x70000000;",
         "/* 搬到 0x71000000：0x70000000 是相簿。相簿開機讀 VBUS，插著線就跳\n"
         " * 來這裡（見 projects/album/app_src/usbdrive.c）。 */\n"
         "__FLASH_BEGIN  = 0x71000000;"),
        ("__FLASH_SIZE   = 0x08000000;",
         "__FLASH_SIZE   = 0x07000000;   /* 從 0x71000000 到 NOR 尾端 */"),
    ], "連結位址", marker="__FLASH_BEGIN  = 0x71000000;")


def patch_media_packet():
    """一次搬多少 bytes。範例預設 512（一個磁區），慢得沒道理。

    MSC 每一輪都是「USB 收 N bytes -> 呼叫一次 BSP_SD_*Blocks_DMA -> 忙等
    SD_TRANSFER_OK」，所以 512 等於每個磁區都付一次完整的來回開銷。

    實測讀取（同一張 512GB exFAT 卡、同一個 110MB 檔案，每次都先重置板子讓
    作業系統丟掉快取才量 —— 不然會量到 1500 MB/s 的假數字）：

        512 bytes  1.74 MB/s
        32 KB      4.60 MB/s
        64 KB      4.59 MB/s   <- 沒有再進步，已到平台期

    取 32KB：跟 64KB 一樣快但省一半 RAM。
    """
    path = os.path.join(PROJ, "Appli", "Inc", "usbd_conf.h")
    _edit(path, [
        ("#define MSC_MEDIA_PACKET     512U",
         "/* 32KB 而不是範例的 512：512 等於每個磁區都付一次 USB+SD 的來回開銷。\n"
         " * 實測 1.74 -> 4.60 MB/s；64KB 沒有再進步，取 32KB 省 RAM。\n"
         " * 緩衝在 USBD_static_malloc 的靜態陣列裡，會自動跟著長大。 */\n"
         "#define MSC_MEDIA_PACKET     32768U"),
    ], "MSC_MEDIA_PACKET", marker="MSC_MEDIA_PACKET     32768U")


def patch_cache_maintenance():
    """SD 讀寫的快取維護。**沒有這個，檔案系統掛不上。**

    範例完全沒有做快取維護，而 BSP_SD_*Blocks_DMA 是 DMA 進可快取的緩衝。
    症狀極度誤導：磁碟正確列舉、容量對、連 MBR 分割表都讀得出來（第一次讀時
    快取是冷的），但 Windows 就是認不出檔案系統 —— 因為之後每次讀 CPU 都拿到
    殘留的快取內容。

    用整體操作而不是 by_addr：bot_data 在 USBD_static_malloc 的陣列裡，只保證
    4 位元組對齊，by_addr 版本對未對齊的範圍會連帶動到相鄰的快取行。
    整體操作約 100us，在這個吞吐量下可以忽略。
    """
    path = os.path.join(PROJ, "Appli", "Src", "usbd_storage_if.c")

    # marker 在這裡特別重要：這兩個樣式植入之後**仍然匹配**（只是在前後加了
    # 東西，原本那幾行還在），沒有 marker 的話重跑會插入第二份。實際踩過。
    _edit(path, [
        # 讀：DMA 前清乾淨、DMA 後失效
        ("    BSP_SD_ReadBlocks_DMA(0, (uint32_t *) buf, blk_addr, blk_len);\n"
         "\n"
         "    /* Wait for Rx Transfer completion */\n"
         "    while (readstatus == 0)\n"
         "    {\n"
         "    }\n"
         "    readstatus = 0;",
         "    /* 讀之前先把這塊緩衝的髒資料寫回並失效，免得 DMA 寫進來之後\n"
         "     * 又被舊的髒快取行蓋掉。 */\n"
         "    SCB_CleanInvalidateDCache();\n"
         "\n"
         "    BSP_SD_ReadBlocks_DMA(0, (uint32_t *) buf, blk_addr, blk_len);\n"
         "\n"
         "    /* Wait for Rx Transfer completion */\n"
         "    while (readstatus == 0)\n"
         "    {\n"
         "    }\n"
         "    readstatus = 0;\n"
         "\n"
         "    /* **DMA 寫進來的資料要讓快取失效，CPU 才看得到。**\n"
         "     * 少了這一行，檔案系統掛不上（見 patch_project.py 的說明）。 */\n"
         "    SCB_InvalidateDCache();"),
        # 寫：DMA 前把資料寫回記憶體
        ("    BSP_SD_WriteBlocks_DMA(0, (uint32_t *) buf, blk_addr, blk_len);",
         "    /* **要寫的資料可能還在快取裡沒下去。** 不先寫回的話 DMA 從記憶體\n"
         "     * 讀到的是舊內容 —— 寫進卡裡的檔案會錯，而且不會有任何錯誤回報。 */\n"
         "    SCB_CleanDCache();\n"
         "\n"
         "    BSP_SD_WriteBlocks_DMA(0, (uint32_t *) buf, blk_addr, blk_len);"),
    ], "SD 讀寫的快取維護", marker="SCB_InvalidateDCache")


def note_no_auto_return():
    """留個記號說明「拔線不會自動回相簿」是已知且刻意的。

    試過三種都不成，細節見 memory 的 h7s78dk-usb-msc-findings：
      - 主迴圈輪詢 VBUS：拔線後 PD 堆疊把主迴圈卡住
      - SysTick 讀 ADC：斷線後 ADC 不再轉換
      - USB Suspend 回呼重置：Suspend 在列舉過程中也會觸發，開機就重置
    使用者決定接受「拔線後按一下 reset」。
    """
    print("  （拔線不會自動回相簿，需按 reset —— 刻意如此，見 tools 的說明）")


def main():
    if not os.path.isdir(PROJ):
        print("找不到 MSC_Standalone：%s" % PROJ)
        print("請先取得 ST 韌體包（各專案的 scripts/setup.sh）")
        return 1

    print("套用隨身碟專案設定：")
    patch_link_address()
    patch_media_packet()
    patch_cache_maintenance()
    note_no_auto_return()
    return 0


if __name__ == "__main__":
    sys.exit(main())
