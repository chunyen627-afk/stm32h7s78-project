# 預編譯燒錄檔：一句話把專案燒回板子

板子拿去開發別的東西之後，用這裡的快照整套燒回來。只需要
STM32CubeProgrammer，**不需要 CubeIDE、也不需要 540MB 的 ST 韌體包**，
而且跟當下的建置環境無關 —— 別的專案把 `cube/` 改成什麼樣都不影響。

```bash
./firmware/restore.sh 相簿           # 電子相簿（含影片播放、USB 隨身碟、USB 耳機）
./firmware/restore.sh 俄羅斯方塊
./firmware/restore.sh 影片燒卡工具   # 獨立影片播放器＋用 ST-LINK 寫 SD 卡
```

名字也認英文與常見說法（`album`、`隨身碟`、`耳機`、`tetris`、`video`…），
完整清單在 `restore.sh` 開頭。

## 專案怎麼分

| 名稱 | 目錄 | 內部 Flash | 外部 `0x70000000` | 外部 `0x71000000` |
|---|---|---|---|---|
| **相簿** | `album/` | `boot.hex`（**有補開 HSE**）| `album.bin` | `usbdrive.bin` |
| **俄羅斯方塊** | `tetris/` | `boot.hex`（原廠）| `tetris.bin` | — |
| **影片燒卡工具** | `video/` | `boot.hex`（原廠）| `video_player.bin` | — |

「影片播放」「USB 隨身碟」「USB 無線耳機」**都是相簿的功能**，不是獨立專案：

- 影片播放：相簿選單的「影片」按鈕
- USB 隨身碟：獨立的第二個 app（`0x71000000`），相簿偵測到 USB1（CN18）
  插上電腦就跳過去，拔線回相簿。**所以燒回相簿一定要連它一起燒** ——
  `projects/video` 的 `--frames` 會把影片寫到 `0x71000000`，正好蓋掉它
- USB 無線耳機：編在 `album.bin` 裡（dongle 插 CN17，JP1 插 `STLK`+`USB2`）

`projects/video` 那個獨立播放器是當初的實驗場，現在主要用途是用 ST-LINK
把影片寫進 SD 卡。`projects/usbaudio` 是開發 USB 耳機時的測試治具，
功能已經整合進相簿，不必燒回。

## 為什麼每次都連 bootloader 一起燒

別的專案常會換掉內部 Flash 的 bootloader。相簿那顆有補開 HSE（USB HS 的
PHY 要用）—— 燒成原廠那顆的話，相簿一切正常、USB 線也會切換過去，
**但電腦完全看不到磁碟機，而且沒有任何錯誤訊息**（踩過，浪費了一輪）。
整套燒多花不到十秒，換來永遠不必想「上次燒的是哪顆」。

## 驗證

腳本把外部 Flash 整份讀回來自己比對，指出不符的位元組在哪。
**不要相信 `STM32_Programmer_CLI -v` 的 Data mismatch**，那可能是假警報
（board-notes 10.5）。現在這塊板子實測外部 Flash 零壞格；舊板子在
`0x70170466`、`0x700217C6` 有固定壞格（都是 bit 2）。

## 這些快照是什麼版本

| 快照 | 原始碼 | 備註 |
|---|---|---|
| `album/` | `ea07784`（2026-08-27）| 含 USB 耳機、±10 秒微調、播放位置斷電記憶 |
| `tetris/` | `ec9b079`（2026-08-24）| |
| `video/` | `3734dab`（2026-08-22）| |

全部編成 **PSRAM 滿速 200MHz**（`shared/xspi_psram.c` 的 `PSRAM_HALF_CLOCK 0`）。
舊板子撐不住 200MHz，症狀是**畫面出現閃爍的細線、靜止畫面也照閃**
（board-notes 10）—— 換回舊板子的話要把那個開關改成 1 重新建置。

## 更新快照

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
