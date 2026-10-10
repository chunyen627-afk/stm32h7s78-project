# 賽車儀表（RaceDash）

PC 玩賽車遊戲時，把板子當成 5 吋副儀表。資料由 PC 上的 **SimHub** 從遊戲讀出，
經 USB 送給板子。

**畫面就是原作**：[caa1211/esp32-gt7-dashboard](https://github.com/caa1211/esp32-gt7-dashboard)
（MIT License）的整個儀表 App —— 資料處理、七個主題、觸控選單、設定畫面 ——
**原始碼一行不改**編進板子，連同它用的繪圖函式庫 LovyanGFX（FreeBSD License）。
原作畫在 320x240，這裡整數放大兩倍到 640x480，置中在 800x480 面板上（左右各 80
像素黑邊），所以配色、字型、間距都跟原作一模一樣。

原作的「PS5 GT7 Wi-Fi 直連」模式不能用 —— 板子沒有 Wi-Fi。

## 七個主題

Classic、GT3（預設）、Retro、Radar、Mono、Pocket、Endurance。

**切換方法照原作**：螢幕亮著時點一下叫出原作的設定畫面，選 **SELECT THEME**。
主題、亮度記在 DTCM，板子重置後還在，**完全斷電會回到 GT3**（原作存在 ESP32 的
NVS，這塊板子還沒接上永久儲存）。

## 接線

**只要一條線：板子的 CN7（ST-LINK）接電腦。** 它在電腦上是
「STMicroelectronics STLink Virtual COM Port」（這台電腦是 COM3），
供電、燒錄、遙測都走這一條。**要直插電腦**，經過 USB 集線器實測認不到。

## SimHub 設定

SimHub 是 PC 上免費的賽車遙測軟體（simhubdash.com），支援 Assetto Corsa、
AC EVO、ACC、iRacing、F1、Forza 等。

1. 左側選單 **Add/remove features**，打開 **Custom serial devices**
   （新版 Settings 裡已經沒有 Plugins 分頁）
2. Custom serial devices → **Add new serial device**：COM 埠選 ST-LINK 那個、Baudrate **115200**
3. Update messages 的 **EDIT** → 打開 **Use javascript** → 把
   [`simhub/racedash-serial.js`](simhub/racedash-serial.js) **整個檔案**貼進去
4. 打開 **Automatic reconnect**
5. **免費版不會自動切換遊戲**：在 SimHub 先選好要玩的遊戲（例如 AC EVO），
   沒選對的話板子收得到資料但一直顯示等待

**設定備份**：[`simhub/CustomSerialPlugin.GeneralSettings2.json`](simhub/CustomSerialPlugin.GeneralSettings2.json)
是設定好的完整檔案。重灌時**先從系統匣的 SimHub 圖示選 Exit SimHub**（關視窗只會
縮到系統匣，而且 SimHub 結束時才寫設定檔，會把複製進去的蓋掉），再把它複製到
`C:\Program Files (x86)\SimHub\PluginsData\Common\`，重開 SimHub。

免費版每秒更新 10 次。

## 架構

```
SimHub ──USB(COM3)──> UART4 ──> race_main.c ──一行 DSH1──> 原作 SHCustomProtocol
                                    │                         │（解析、主題、觸控 UI）
                                    │                         ▼
                                    │                LovyanGFX 320x240 記憶體畫布
                                    └── 放大兩倍貼到 framebuffer <───┘
```

| 位置 | 內容 |
|---|---|
| `app_src/race_main.c` | 螢幕、序列埠、放大貼圖、觸控座標換算、背光、設定儲存、PSRAM 堆積 |
| `app_src/serial.c` | UART4 中斷接收、逐行取出 |
| `gt7dash/orig/` | **原作檔案，不修改**（SHCustomProtocol.h、七個主題、SimHub 解析、GT7 解析器…）|
| `gt7dash/lgfx/` | LovyanGFX（只取記憶體畫布與字型），三處修補見下 |
| `gt7dash/compat/` | Arduino／ESP32 相容層：String、Preferences（NVS）、Wi-Fi 空殼、`tft` 畫布 |
| `gt7dash/port.cpp` | 銜接層：對 C 開 `gt7dash_init/feed/step/canvas` |
| `gt7dash/build_lib.sh` | 用同一套 ARM 編譯器把上面編成 `libgt7dash.a` |
| `gt7dash/host/host_dash.cpp` | 電腦端參考渲染器：同一份原作程式碼在電腦上畫成圖，跟板子比對 |

CubeIDE 專案是 C 專案，不會編 C++。原作那一塊另外編成函式庫，由
`scripts/build.sh` 從 make 命令列帶進連結（`USER_OBJS` / `LIBS`），不必改專案檔。

**LovyanGFX 的修補**（都只在 `LGFX_RACEDASH` 時生效，其他平台不受影響）：
1. `platforms/racedash/`：新平台，只提供計時與記憶體配置（不接任何面板）
2. `lgfx_fonts.cpp`：排除 120MB 的中日韓點陣字型（儀表用不到）
3. `LGFXBase.hpp`：有相容層的 `String` 時打開 `drawString(String, ...)`

**踩過的坑**：
- 相容層的 `min`/`max` 第一版回傳參照（`decltype(a < b ? a : b)` 在兩個同型別參數時
  推成參照），回傳的是已經消失的參數 —— GT3 的檔位算出亂數座標、整個不見
- 範本 `sysmem.c` 的 `_sbrk` 以為堆疊緊接在堆積後面，但這個範本的堆疊在 DTCM，
  上限比堆積起點還低，**任何 malloc 都失敗**。`patch_project.py` 把它停用，改用
  `race_main.c` 放在 PSRAM（0x90400000 起）的版本

## 不開遊戲也能看

- **展示模式**：SWD 寫 `0x2000404C = 0x44454D4F`（"DEMO"），板子自己產生跑圈資料
- **假 SimHub**：`powershell -ExecutionPolicy Bypass -File tools/fake_simhub.ps1`
  （SimHub 開著時 COM 埠被佔，用不了）

## 建置與燒錄

```bash
./scripts/setup.sh      # 第一次：從 Template_XIP 建 CubeIDE 專案
./scripts/build.sh      # 先編原作函式庫，再編專案（改過原始碼清單要 FORCE_IDE=1）
./scripts/flash.sh      # 燒錄（含原廠 bootloader）
```

## 量到的數字

- 原作畫圖＋放大貼圖：一格約 11.5ms，面板 60Hz 下每秒約 60 格
- 堆積（PSRAM）：約 192KB（主畫布 150KB ＋主題用的小畫布、String）
- SimHub 實機：AC EVO，原作解析器 0 行無效

## 除錯

狀態寫在 DTCM `0x20004020`（SWD `mode=HOTPLUG` 讀，不會重置板子）：

| 格 | 內容 |
|---|---|
| 0 | 魔術字 `0x52445348`（"RDSH"）|
| 1 / 2 | tick / 收到的位元組 |
| 3 / 4 | 原作判為有效 / 無效的行 |
| 5 / 7 | 畫了幾格 / 一格多少 CPU 週期 |
| 8 / 9 | 序列埠溢位 / 太長被丟的行 |
| 10 | 堆積用了多少 |
| 11 | 寫 "DEMO" 開展示模式 |
| 12 | 寫 `0x54480000 \| 主題` 切主題（0 Classic、1 GT3、2 Retro、3 Radar、4 Mono、5 Pocket、6 Endurance）|
| 15 | 寫 "FROZ" 停止貼圖，截圖用 |

**截圖要先凍結**，不然讀到的是很多格混在一起。設定（主題、亮度）存在 DTCM `0x20005000`。

## 下一步（使用者要求的順序：先一模一樣，再優化）

高解析度、去鋸齒：原作座標與排版不變，但改用 2 倍解析度、抗鋸齒重畫文字與圓弧。
