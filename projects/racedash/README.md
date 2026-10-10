# 賽車儀表（RaceDash）

PC 玩賽車遊戲時，把板子當成 5 吋副儀表。資料由 PC 上的 **SimHub** 從遊戲讀出，
經 USB 送給板子。

**畫面就是原作**：[caa1211/esp32-gt7-dashboard](https://github.com/caa1211/esp32-gt7-dashboard)
（MIT License）的整個儀表 App —— 資料處理、七個主題、觸控選單、設定畫面 ——
**原始碼一行不改**編進板子，連同它用的繪圖函式庫 LovyanGFX（FreeBSD License）。
原作畫在 320x240；板子顯示的是**同一個畫面的 2 倍抗鋸齒版本**（640x480，置中在
800x480 面板上，左右各 80 像素黑邊）：排版、配色、字距照原作，文字用原始字型的
輪廓重畫、圓和斜線抗鋸齒，不再是放大的方塊（見下面「高解析度」）。

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
                                    │                         │ 每次繪圖同時畫一份
                                    │                         ▼
                                    │                2 倍抗鋸齒鏡像 640x480（hires/）
                                    └── 改過的列貼到 framebuffer <───┘
```

| 位置 | 內容 |
|---|---|
| `app_src/race_main.c` | 螢幕、序列埠、貼圖（只貼改過的列）、觸控座標換算、背光、設定儲存、PSRAM 堆積 |
| `gt7dash/hires/` | 2 倍抗鋸齒鏡像：`hires.cpp`、字型輪廓 `hires_fonts.inc`（由 `tools/gen_hires_fonts.c` 從 TTF 產生）、stb_truetype |
| `app_src/serial.c` | UART4 中斷接收、逐行取出 |
| `gt7dash/orig/` | **原作檔案，不修改**（SHCustomProtocol.h、七個主題、SimHub 解析、GT7 解析器…）|
| `gt7dash/lgfx/` | LovyanGFX（只取記憶體畫布與字型），三處修補見下 |
| `gt7dash/compat/` | Arduino／ESP32 相容層：String、Preferences（NVS）、Wi-Fi 空殼、`tft` 畫布 |
| `gt7dash/port.cpp` | 銜接層：對 C 開 `gt7dash_init/feed/step/canvas` |
| `gt7dash/build_lib.sh` | 用同一套 ARM 編譯器把上面編成 `libgt7dash.a` |
| `gt7dash/host/` | 電腦端參考渲染器：同一份原作程式碼在電腦上畫成圖（原本的與 2 倍的各一張），跟板子比對 |

CubeIDE 專案是 C 專案，不會編 C++。原作那一塊另外編成函式庫，由
`scripts/build.sh` 從 make 命令列帶進連結（`USER_OBJS` / `LIBS`），不必改專案檔。

**LovyanGFX 的修補**（都只在 `LGFX_RACEDASH` 時生效，其他平台不受影響）：
1. `platforms/racedash/`：新平台，只提供計時與記憶體配置（不接任何面板）
2. `lgfx_fonts.cpp`：排除 120MB 的中日韓點陣字型（儀表用不到）
3. `LGFXBase.hpp`：有相容層的 `String` 時打開 `drawString(String, ...)`
4. 高解析度鏡像的掛勾（`LGFXBase.hpp/.cpp`、`LGFX_Sprite.hpp`）：記憶體畫布改用會同時
   寫鏡像的子類別；圓、圓角框、三角形、線、字元繪製的開頭各一行；畫布建立、刪除、
   互貼時配對鏡像。不定義 `LGFX_RACEDASH` 時編出來跟原本完全一樣

## 高解析度

原作的程式碼照舊畫在 320x240 的畫布上 —— 排版、量字寬、讀像素都看那一張，所以
行為跟原作一模一樣。每張畫布另外配一張 2 倍大的鏡像，板子顯示的是鏡像：

- **長方形、水平垂直線、貼圖**：放大兩倍寫進鏡像（本來就沒有鋸齒）。沒有特別處理的
  繪圖也走這條，所以不會漏畫，最差就是跟以前一樣
- **圓、圓角框、三角形、斜線**：把原本 1 倍的幾何換算成 2 倍（1 倍的像素 (x,y) 佔
  2 倍的 [2x,2x+2)），用 stb_truetype 的掃描線算面積覆蓋率，畫抗鋸齒。原作的圓弧
  是一串端點四捨五入的短線段，放大會一節一節扭動，所以同色的連續短線段會先收集
  起來、把進位誤差平均掉再一起畫
- **FreeSans、DejaVu 的字**：原作的點陣字就是從這兩套字型轉出來的（FreeSans 是
  Adafruit fontconvert 以 141 dpi 轉，DejaVu 字型名稱的數字就是像素大小；實測跟
  原點陣字形重疊 0.9 以上，點陣字比輪廓低 1 列）。每個字的位置、字距照原本 1 倍
  算出來的，字形換成輪廓、以 2 倍大小畫抗鋸齒。原作用 `setTextSize(0.72)` 這類
  非整數縮放時，原本會掉行掉列（例如 Retro 的「0」少一邊像「D」），輪廓不會
- **沒有輪廓的點陣字（Font0/2/4/7）**：沿像素邊界描成輪廓，連續兩個以上的 45° 階梯
  換成斜線，字本來的直角保持方正（Classic 放大 8 倍的檔位字「4」斜筆變直、橫筆
  尾巴還是方的）
- **Pocket 的大字**是原作故意用方塊拼的像素風，維持原樣

字型輪廓只取 ASCII，由 `hires/tools/gen_hires_fonts.c` 從 TTF 產生（TTF 不在 repo
裡，出處見 `gt7dash/NOTICE.md`）。電腦端驗證：

```bash
./gt7dash/host/build_host.sh                 # 用 w64devkit 編
gt7dash/build/host/host_dash 1 "DSH1;..." 40 160,120   # 主題、一行資料、跑幾格、點擊
```

輸出 `theme1.ppm`（原本的 320x240）與 `theme1_hi.ppm`（板子顯示的 640x480）。

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

- 每秒 60 格（面板 60Hz）。資料沒變的格約 1ms（原作只重畫有變的部分，貼圖只貼改過的列）
- 展示模式（每 0.1 秒所有數值都變）最慢的一格：GT3 31ms、Classic 24ms、Radar 23ms、
  Endurance 22ms、Mono 15ms —— 收到資料的那一格超過 16.7ms，其餘照常 60 格；
  免費版 SimHub 每秒 10 次，看不出來。換主題後第一次畫要產生字形，會慢一次
- 堆積（PSRAM）：主畫布與鏡像約 1.2MB，七個主題都看過約 2.2MB（上限 12MB）
- SimHub 實機：AC EVO，原作解析器 0 行無效

## 除錯

狀態寫在 DTCM `0x20004020`（SWD `mode=HOTPLUG` 讀，不會重置板子）：

| 格 | 內容 |
|---|---|
| 0 | 魔術字 `0x52445348`（"RDSH"）|
| 1 / 2 | tick / 收到的位元組 |
| 3 / 4 | 原作判為有效 / 無效的行 |
| 5 / 7 | 畫了幾格 / 一格多少 CPU 週期（600MHz）|
| 6 / 14 | 這格貼了幾列 / 其中貼圖的週期 |
| 8 / 9 | 序列埠溢位 / 太長被丟的行 |
| 10 | 堆積用了多少 |
| 11 | 寫 "DEMO" 開展示模式 |
| 12 | 寫 `0x54480000 \| 主題` 切主題（0 Classic、1 GT3、2 Retro、3 Radar、4 Mono、5 Pocket、6 Endurance）|
| 13 | 寫 "NOHI"（`0x4E4F4849`）改貼原本 320x240 的放大，跟抗鋸齒版比對 |
| 15 | 寫 "FROZ" 停止貼圖，截圖用 |
| 16 | 開機以來最慢的一格（週期），寫 0 重新量 |

**截圖要先凍結**，不然讀到的是很多格混在一起。設定（主題、亮度）存在 DTCM `0x20005000`。

## 還沒做

- 設定（主題、亮度）完全斷電會回到預設：還沒接上永久儲存
- 授權版 SimHub 每秒 60 次時每格都有新資料，換檔那種大範圍重畫會掉一兩格（見上面的數字）
