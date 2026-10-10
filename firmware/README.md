# 可以燒回板子的專案

板子拿去開發別的東西之後，用這裡的快照**一行整套燒回來**。只需要
STM32CubeProgrammer（CubeIDE 內附），不需要 540MB 的 ST 韌體包、不必建置，
而且跟當下的建置環境無關 —— 別的專案把 `cube/` 改成什麼樣都不影響。

```bash
./firmware/restore.sh 相簿
./firmware/restore.sh 俄羅斯方塊
./firmware/restore.sh               # 不給名字：列出目前所有可燒回的專案
```

板子要**直接插電腦**（CN7，ST-LINK 那個孔）—— 經過 USB 集線器實測認不到。

---

## 目前有的專案

### 相簿　`restore.sh 相簿`

日常在用的那一個。也可以說：影片、影片播放、隨身碟、耳機、album。

| 功能 | 說明 |
|---|---|
| 照片輪播 | 勾選資料夾（含所有子層）、隨機或循序、間隔 2~5 秒、直立／橫向／自動方向 |
| 我的最愛 | 暫停時點愛心收藏，選單「最愛」只播收藏的；清單存卡上 `我的最愛.txt` |
| 影片播放 | 選單「影片」，卡上的 `*.bin` 影格包，無限循環 |
| ├ 音軌 | 同名 `.wav` 一起播，3.5mm 耳機孔（CN16），畫面上調音量 |
| ├ USB 無線耳機 | 2.4GHz dongle 插 CN17 自動改走無線，3.5mm 照樣保留 |
| ├ 進度控制 | 拖進度條、`-10s` / `+10s` 微調 |
| └ 斷電記憶 | 每部影片看到哪記在卡上 `播放位置.txt`，拔電再開機接著播 |
| USB 隨身碟 | USB1（CN18）插電腦就變隨身碟，拔線自動回相簿 |
| 觸控操作 | 全觸控介面；USER 鍵短按調背光、長按暫停 |

硬體：USB 耳機要 JP1 插 `STLK`+`USB2`、dongle 插 CN17（這樣接時 CN17 不能再插
電腦或充電器）。冷上電後 dongle 要重插一次才有聲音（已知，決定不修）。

燒錄內容：bootloader（有補開 HSE）＋ 相簿 `0x70000000` ＋ 隨身碟 `0x71000000`。

### 俄羅斯方塊　`restore.sh 俄羅斯方塊`

直立版、繁體中文介面、超任風格觸控手把（十字鍵＋A/B/X/Y）。SRS 旋轉含踢牆、
7-bag、幽靈落點、保留、20 級重力。也可以說：方塊、tetris。

### 影片燒卡工具　`restore.sh 影片燒卡工具`

獨立的影片播放器，主要用途是**用 ST-LINK 把影片寫進 SD 卡**（不必拔卡）。
平常看影片請用相簿。也可以說：影片播放器、燒卡、video。

> 不在清單上的：`projects/usbaudio` 是開發 USB 耳機時的測試治具，功能已經
> 整合進相簿，不必單獨燒。

---

## 新增一個專案

1. 在 `firmware/` 開一個資料夾，例如 `firmware/clock/`
2. 放進 `boot.hex`（bootloader）和要燒到外部 Flash 的 `.bin`
   （用 `arm-none-eabi-objcopy -O ihex` / `-O binary` 從 ELF 轉）
3. 寫一份 `project.conf`（照抄 `tetris/project.conf` 改內容）：

   ```bash
   TITLE="時鐘"                          # 列表上顯示的名字
   DESC="一句話說明"
   ALIASES="時鐘 鬧鐘 clock"             # 使用者可能怎麼叫它，空白分隔
   ITEMS="clock.bin:0x70000000"          # 外部 Flash：檔名:位址，可以多個
   SOURCE="<提交> (<日期>)"              # 快照從哪個版本編出來的
   ```
4. 跑一次 `./firmware/restore.sh` 確認列出來了，再實際燒一次驗證
5. `git ls-files firmware` 確認 `.bin` / `.hex` 有進版控
   （`.gitignore` 預設排除它們，`firmware/**` 有例外）

`restore.sh` 本身不用改。

## 驗證

腳本把外部 Flash 整份讀回來自己比對，指出不符的位元組在哪。
**不要相信 `STM32_Programmer_CLI -v` 的 Data mismatch**，那可能是假警報
（board-notes 10.5）。現在這塊板子實測外部 Flash 零壞格；舊板子在
`0x70170466`、`0x700217C6` 有固定壞格（都是 bit 2）。

## 快照的版本與 PSRAM 時脈

每份快照是從哪個提交編出來的，寫在各自 `project.conf` 的 `SOURCE`，
燒錄時也會印出來。

全部編成 **PSRAM 滿速 200MHz**（`shared/xspi_psram.c` 的 `PSRAM_HALF_CLOCK 0`）。
舊板子撐不住 200MHz，症狀是**畫面出現閃爍的細線、靜止畫面也照閃**
（board-notes 10）—— 換回舊板子的話要把那個開關改成 1 重新建置。

## 更新既有專案的快照

改了某個專案、使用者也用過覺得沒問題之後，才把新版本存進來（`album.bin`
有 1.6MB，每次建置都提交會讓倉庫迅速膨脹）：

```bash
GCC=<CubeIDE>/plugins/...gnu-tools-for-stm32.../tools/bin
P=cube/Projects/STM32H7S78-DK

# 相簿（先 cd projects/album && ./scripts/build.sh）
$GCC/arm-none-eabi-objcopy -O ihex   $P/Templates/Album/STM32CubeIDE/Boot/Debug/Template_XIP_Boot.elf firmware/album/boot.hex
$GCC/arm-none-eabi-objcopy -O binary $P/Templates/Album/STM32CubeIDE/Appli/Debug/Album_Appli.elf     firmware/album/album.bin
$GCC/arm-none-eabi-objcopy -O binary $P/Applications/USB_Device/MSC_Standalone/STM32CubeIDE/Appli/Debug/MSC_Standalone_Appli.elf firmware/album/usbdrive.bin

# 俄羅斯方塊（先 cd projects/tetris && ./scripts/build.sh）
$GCC/arm-none-eabi-objcopy -O binary $P/Templates/Tetris/STM32CubeIDE/Appli/Debug/Template_XIP_Appli.elf firmware/tetris/tetris.bin
```

**燒進去之前確認治具開關都是正式值**：`album_main.c` 的 `ALBUM_AUTOPLAY 0`、
`ALBUM_AUTOSEEK_MS 0u`、`ALBUM_DCACHE_OFF 0`、`ALBUM_USB_AUDIO 1`；
`usbaudio.c` 的 `USB_PUMP_ENABLE 1`。測完忘了開回來踩過一次（USB 耳機
沒聲音、一直斷線重連）。

## 新板子的硬體前提

- **SW1（BOOT0 滑動開關）在 0 的位置**（撥到 1 會停在 ST 原廠 bootloader，
  程式本體永遠不會執行）
- Option Bytes：`XSPI1_HSLV=1`、`XSPI2_HSLV=1`、`VDDIO_HSLV=0`
  （出廠通常已正確，用 `STM32_Programmer_CLI -c port=SWD -ob displ` 確認）

## 內容不在這裡的東西

- **照片**：放進 SD 卡（**FAT32 或 exFAT**），相簿會遞迴掃描
- **`我的最愛.txt`、`播放位置.txt`**：不用自己準備，韌體第一次用到時會在
  卡的根目錄建立。純文字，電腦上打得開
- **影片**：`*.bin` 放 SD 卡根目錄，用 `projects/video/tools/video2bin.py`
  從 mp4 轉出來；同名 `.wav` 是音軌。太大不進版控
- **中文字型** `font_zh.c`：6.4MB 的產生物，`scripts/setup.sh` 會現場產生
