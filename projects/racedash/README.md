# 賽車儀表（RaceDash）

PC 玩賽車遊戲時，把板子當成 7 吋的副儀表：換檔燈、檔位、車速、轉速、
圈速、油量、TC／ABS、胎溫、油門煞車。資料由 PC 上的 **SimHub** 從遊戲讀出，
經 USB 送給板子。

移植自 [caa1211/esp32-gt7-dashboard](https://github.com/caa1211/esp32-gt7-dashboard)
（MIT License）的 SimHub 模式：資料格式（DSH1）與 SimHub 公式沿用，畫面照
800x480 重新排版。原專案的「PS5 GT7 Wi-Fi 直連」模式沒有移植 —— 這塊板子沒有 Wi-Fi。

## 接線

**只要一條線：板子的 CN7（ST-LINK）接電腦。** 它在電腦上會出現一個
「STMicroelectronics STLink Virtual COM Port」（這台電腦是 COM3），
供電、燒錄、遙測都走這一條。**要直插電腦**，經過 USB 集線器實測認不到。

## SimHub 設定

SimHub 是 PC 上免費的賽車遙測軟體，支援 Assetto Corsa、ACC、iRacing、
F1、Forza 等大部分 PC 賽車遊戲（官網 simhubdash.com）。

1. 設定 → Plugins，啟用 **Custom Serial devices**，左側選單會多出這一項
2. Custom Serial devices → 新增裝置：
   - Serial port：ST-LINK 的 COM 埠
   - Baudrate：**115200**
3. Update messages → 新增一則：
   - 勾 **Use JavaScript**
   - 把 [`simhub/racedash-serial.js`](simhub/racedash-serial.js) **整個檔案**貼進去
   - 頻率選 10Hz（免費版上限）
4. 開遊戲，板子會從等待畫面切到儀表

板子的等待畫面會告訴你卡在哪一步：

| 畫面顯示 | 意思 |
|---|---|
| 還沒收到任何資料 | SimHub 沒開、COM 埠選錯、或鮑率不是 115200 |
| 有收到資料但格式不對 | 公式沒貼完整，或沒勾 Use JavaScript |
| 連線中，遊戲目前沒有在跑 | 一切正常，進賽道就會出現 |

最下面一行 `RX / OK / BAD` 是收到的位元組數、有效行數、無效行數。

**免費版每秒更新 10 次**，車速、檔位夠用，轉速條會有點跳；SimHub 付費版可以更快。

## 不開遊戲也能看：假 SimHub

```bash
powershell -ExecutionPolicy Bypass -File tools/fake_simhub.ps1              # 送 60 秒模擬跑圈資料到 COM3
powershell -ExecutionPolicy Bypass -File tools/fake_simhub.ps1 -Seconds 0   # 一直送
powershell -ExecutionPolicy Bypass -File tools/fake_simhub.ps1 -Bad         # 混入壞資料，驗證不會被弄亂
```

SimHub 跟這個工具不能同時開同一個 COM 埠。

## 建置與燒錄

```bash
./scripts/setup.sh      # 第一次：從 Template_XIP 建 CubeIDE 專案
./scripts/build.sh      # 編譯（改過 tools/patch_project.py 的原始碼清單要加 FORCE_IDE=1）
./scripts/flash.sh      # 燒錄（含原廠 bootloader）
```

| 檔案 | 內容 |
|---|---|
| `app_src/race_main.c` | 初始化、主迴圈、雙緩衝換頁 |
| `app_src/serial.c` | UART4（ST-LINK 虛擬 COM 埠）中斷接收、逐行取出 |
| `app_src/dsh1.c` | DSH1 解析與驗證（壞的一行不會蓋掉畫面上的好資料）|
| `app_src/dash.c` | 畫面 |
| `core/numfont.c` | 大數字字型，`tools/gennum.py` 用 Windows 內建的 Bahnschrift 產生（4 bit 灰度、平滑邊緣）|
| `core/font_zh.c` | 標籤用的中文字型，`tools/genfont.py` 自動掃描畫面用到的字 |
| `test/dsh1_test.c` | 解析器的電腦端測試：`gcc -I../app_src dsh1_test.c ../app_src/dsh1.c && ./a.exe` |

## 量到的數字

- 通訊：送 137 行（含 19 行故意的壞資料），板子判 118 行有效、19 行無效，一行不差，溢位 0
- 畫一格約 15ms（600MHz），在面板 60Hz 的 16.7ms 預算內

## 除錯

狀態寫在 DTCM `0x20004020`（用 SWD `mode=HOTPLUG` 讀，不會重置板子）：

| 格 | 內容 |
|---|---|
| 0 | 魔術字 `0x52445348`（"RDSH"）|
| 1 | tick |
| 2 / 3 / 4 | 收到的位元組 / 有效行 / 無效行 |
| 5 | 畫了幾格 |
| 6 | 最後一行的序號 |
| 7 | 畫一格花幾個 CPU 週期 |
| 8 / 9 | 序列埠溢位 / 太長被丟的行 |
| 15 | 寫 `0x5A4F5246`（"FROZ"）就停止重畫，截圖用 |

**截圖要先凍結。** 從板子讀一整張畫面要將近一秒，期間儀表重畫幾十次
（每次先清黑再畫），沒凍結的話讀到的是很多格混在一起的橫紋 —— 看起來像
螢幕壞了，其實是截圖的問題。
