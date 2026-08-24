# STM32H7S78-DK 專案集

這塊板子上的各種東西，共用同一份 ST 韌體包與同一份板子筆記。

## 專案

| 專案 | 說明 |
|---|---|
| [projects/tetris](projects/tetris) | 直立版俄羅斯方塊，繁體中文介面，超任風格觸控手把 |
| [projects/album](projects/album) | 電子相簿，讀 SD 卡照片輪播（FAT32／exFAT），全觸控操作、我的最愛，也能播 MJPEG 影片 |
| [projects/video](projects/video) | MJPEG 影片播放與工具鏈：mp4 轉檔、SD 卡／外部 Flash 兩種來源、透過 ST-LINK 寫卡 |
| [projects/usbdrive](projects/usbdrive) | 隨身碟模式：USB1 插上電腦就把 SD 卡直接掛給電腦，不用拔卡 |

## 目錄結構

```
docs/board-notes.md   板子層級的坑（所有專案共用，遇到問題先翻這裡）
tools/sd-check.ps1    SD 卡好壞的快速判斷（見下）
projects/video/tools/video2bin.py   影片轉板子能播的 .bin（含拖放用的 .bat）
projects/usbdrive/                  隨身碟模式（相簿的第二個 app，燒在 0x71000000）
shared/               跨專案共用的程式碼
  gfx.c / gfx.h         直立繪圖層（面板 800x480 橫式，轉成 480x800 用）
  xspi_psram.c          PSRAM 降頻（這塊板子的個體差異補償，見下）
projects/<name>/      各專案：app_src/ core/ scripts/ tools/
cube/                 ST 韌體包 STM32CubeH7RS（gitignore，所有專案共用）
```

`cube/` 有 540MB，只放一份。任何專案的 `scripts/setup.sh` 都會抓進來，
已經有了就跳過。

## 換新板子：直接燒預編譯檔

不想裝 540MB 的韌體包與 CubeIDE 的話，[firmware/](firmware) 裡有可以直上的
燒錄檔：

```bash
./firmware/flash.sh --boot     # 新板子第一次：bootloader + 相簿
./firmware/flash.sh            # 之後只更新相簿
```

硬體前提、PSRAM 時脈要不要改回 200MHz，見 [firmware/README.md](firmware/README.md)。

## 建置

```bash
cd projects/<name>
./scripts/setup.sh          # 建立 CubeIDE 專案（每個專案只需一次）
./scripts/build.sh          # 編譯
./scripts/flash.sh --boot   # 第一次燒錄，含 bootloader
./scripts/flash.sh          # 之後只更新應用程式
```

`build.sh` 有 makefile 就直接 `make`，沒有才叫 CubeIDE headless 產生。
改過 `tools/patch_project.py` 的原始碼清單之後要重新產生 makefile：
把 `Debug/` 刪掉，或設 `FORCE_IDE=1`。

## 這塊板子的兩個硬體問題

詳見 [docs/board-notes.md](docs/board-notes.md)，摘要：

**PSRAM 撐不住 BSP 預設的 200MHz**。時序餘裕有板子個體差異，這塊落在錯誤側，
會偶發單一位元讀取錯誤。framebuffer 在 PSRAM 裡 → 畫面閃爍；資料結構在
PSRAM 裡 → 字串莫名其妙壞掉。

`shared/xspi_psram.c` 覆寫 BSP 的 `__weak MX_XSPI_RAM_Init()` 減半成 100MHz。
**這是板子個體差異的補償，不是所有板子都需要**，所以做成各專案自行選用：
album 有納入建置，tetris 沒有。畫面會閃就把它加進該專案的原始碼清單。

**外部 NOR Flash 有壞格，而且壞的都是 bit 2**（比較像某條位元線在特定區塊
偏弱，不是隨機耗損）。目前觀察到三個位址：`0x70170466`、`0x700217C6`、
`0x7017B386`，全部落在 FatFs 的查表資料（`oem2uni*`）裡，無害。

注意**觀察到的位址會隨韌體版面改變**：壞格只有在「該位址的正確值需要
bit 2 = 1」時才會顯示成不符，所以換一版程式就可能冒出新的位址，
也可能舊的不再出現。重點不是背這幾個數字，而是**每次燒錄都讀回來比對**。

`flash.sh` 會整份讀回自己比對，並指出不符的位元組落在哪個符號 ——
落在查表資料可以忽略，**落在 `.text` 的函式不行**，要調整版面避開。
不要相信 `STM32_Programmer_CLI -v` 的 Data mismatch，那可能是假警報，
一定要另外讀回來確認（board-notes 10.5）。

## 記憶卡壞了沒？

```powershell
.	ools\sd-check.ps1 -DriveLetter F              # 預設寫 2GB
.	ools\sd-check.ps1 -DriveLetter F -FillAll     # 用光可用空間
```

**判斷一張卡好壞，唯一有效的方法是寫夠多。** 讀取正常不代表什麼，小量寫入
正常也不代表什麼 —— 實際踩過的那張 8GB：PC 上讀 4493 個檔案零錯誤、
刪檔建資料夾 chkdsk 全正常，但連續寫 188 秒就從 USB 匯流排消失。

腳本不需要管理員權限、不動既有資料。板子上還有更敏感的版本
（相簿韌體的 `g_dbg_wrtest`，壞卡 15 次寫入就現形）。
判讀方式與實測基準見 [docs/board-notes.md](docs/board-notes.md) 20.15。

## 除錯慣例

板子上沒有 UART 主控台，狀態一律用**全域變數 + SWD 讀取**：

```bash
CLI=".../STM32_Programmer_CLI.exe"
NM=".../arm-none-eabi-nm.exe"
A=$("$NM" app.elf | grep " g_stage$" | awk '{print "0x"$1}')
"$CLI" -c port=SWD mode=HOTPLUG -r32 $A 0x4
```

**燒錄用 `mode=UR`、觀察執行中的狀態用 `mode=HOTPLUG`**，不要混用：
UR 每次連線都會 halt-on-reset，程式根本沒機會跑。
