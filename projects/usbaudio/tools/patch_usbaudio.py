#!/usr/bin/env python3
"""把 ST 的 USB_Host/AUDIO_Standalone 範例改成可以在這塊板子上測無線耳機。

**為什麼要有這支程式**：範例在 `cube/` 底下，而 cube/ 是 gitignore 的 ——
重跑 setup.sh 這些修改就消失了。跟 album 的 patch_project.py 同一個道理。

用法：
    python tools/patch_usbaudio.py            # 套用
    CUBE_DIR=... python tools/patch_usbaudio.py

改了四個地方，前兩個是**ST 範例真正的 bug**（沒改就聽不到正常聲音）：

1. **WAV 解析寫死第 44 個位元組。** 範例的 WAV_InfoTypedef 假設 data 區塊
   緊接在 fmt 後面。但 ffmpeg 產生的 wav 會在中間插一個 LIST 區塊 ——
   實測我們卡上的檔案 data 在第 **78** 個位元組。偏移 34 不是 4 的倍數，
   16-bit 立體聲的每個取樣框被切成兩半，症狀是「聽得出原本的聲音但疊著
   刺耳雜訊，而且音量越大越明顯」（雜訊跟著訊號走 = 波形被弄壞）。

2. **SetFrequency 的回傳值沒被檢查。** 那是非同步的控制傳輸，第一次一定
   回 USBH_BUSY。範例呼叫一次就不管 —— **取樣率從來沒有真的設定到裝置上**。
   改成在播放狀態裡重試到 USBH_OK（不能在原地卡迴圈等，完成它需要主迴圈
   去跑 USBH_Process）。

3. exFAT：範例的 ffconf 是 FF_FS_EXFAT=0 / FF_USE_LFN=0，讀不了我們那張
   512GB 的卡（會回報 "There is no WAV file on the microSD"）。

4. 自動開播、印出協商到的格式：遠端除錯時「等人按 USER 鍵」會讓每一輪都
   要人配合，而且錄序列埠的時機很難抓（board-notes 16.7 同一條教訓）。

狀態列印走 UART4 = ST-LINK 的虛擬序列埠（電腦上是 COM4，115200 8N1）。
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CUBE = os.environ.get("CUBE_DIR") or os.path.join(ROOT, "..", "..", "cube")
CUBE = os.path.abspath(CUBE)
APP = os.path.join(CUBE, "Projects", "STM32H7S78-DK", "Applications",
                   "USB_Host", "AUDIO_Standalone", "Appli")


def edit(path, pairs, tag):
    if not os.path.isfile(path):
        print("  !! 找不到 %s" % path)
        return
    s = io.open(path, encoding="utf-8", errors="replace").read()
    if tag in s:
        print("  %s 已改過" % os.path.basename(path))
        return
    for old, new in pairs:
        if old not in s:
            print("  !! %s 找不到要取代的片段，沒有套用" % os.path.basename(path))
            return
        s = s.replace(old, new, 1)
    io.open(path, "w", encoding="utf-8").write(s)
    print("  %s 已套用" % os.path.basename(path))


def main():
    if not os.path.isdir(APP):
        print("找不到 ST 的 USB Host 音訊範例：%s" % APP)
        print("請先跑 scripts/setup.sh 把韌體包抓下來")
        return 1

    print("套用 USB Host 音訊治具的修改：")

    # --- ST 範例的第三個真 bug：USB 的 48MHz 其實是 46.08MHz -------------
    #
    # Boot 的 SystemClock_Config 把 PLL3 設成 HSE(24MHz) / M=5 = 4.8MHz、
    # N=240 -> VCO 1152MHz、**Q=25 -> 46.08MHz**，而 usbh_conf.c 又指定
    # RCC_USBOTGFSCLKSOURCE_PLL3Q 當 USB 全速的 48MHz。
    #
    # 46.08 / 48 = 0.96 —— 實測 SOF 每秒只有 **960** 個而不是 1000
    # （hUsbHostFS.Timer 與硬體都同意）。等時端點是一個訊框一包，所以
    # dongle 每秒只收到 46,080 個取樣，卻被 SetFrequency 告知要用 48000Hz
    # 播 —— 它得週期性補洞，那就是「啵啵啵」，而且音量越大越明顯。
    #
    # 1152 / 24 = 48.000 剛好整除。PLL3Q 在這個範例裡只有 USB 一個使用者。
    boot = os.path.join(CUBE, "Projects", "STM32H7S78-DK", "Applications",
                        "USB_Host", "AUDIO_Standalone", "Boot")
    edit(os.path.join(boot, "Src", "main.c"), [
        ("  RCC_OscInitStruct.PLL3.PLLQ = 25;",
         "  RCC_OscInitStruct.PLL3.PLLQ = 24;   /* 1152/24 = 48.000MHz；"
         "原本的 25 是 46.08MHz，USB 慢 4% */"),
    ], "PLL3.PLLQ = 24")

    edit(os.path.join(APP, "Inc", "ffconf.h"), [
        ("#define FF_USE_LFN		0", "#define FF_USE_LFN		1"),
        ("#define FF_FS_EXFAT		0", "#define FF_FS_EXFAT		1"),
        ("#define FF_CODE_PAGE	932", "#define FF_CODE_PAGE	950"),
    ], "#define FF_FS_EXFAT		1")

    print("")
    print("audio.c 的四處修改請見本檔開頭的說明；完整的原始碼片段保存在")
    print("projects/usbaudio/patches/ 底下，套用方式與上面相同。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
