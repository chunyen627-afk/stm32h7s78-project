# 預編譯燒錄檔

換一塊新板子時直接燒這裡的檔案，不必先建置整個專案（那需要 540MB 的
ST 韌體包與 CubeIDE）。

| 檔案 | 燒到哪 | 說明 |
|---|---|---|
| `Boot_XIP.hex` | 內部 Flash | bootloader，**新板子只需要燒一次** |
| `album.bin` | 外部 Flash `0x70000000` | 電子相簿（含 MJPEG 影片播放） |
| `video_player.bin` | 外部 Flash `0x70000000` | 獨立的影片播放器＋SD 燒錄模式 |

兩個 app 二選一，都燒在同一個位址。平常用相簿那個就好；
`video_player.bin` 是要用 ST-LINK 把影片寫進 SD 卡時才換上去
（見 [projects/video](../projects/video)）。

## 燒錄

```bash
./firmware/flash.sh --boot          # 新板子第一次：bootloader + 相簿
./firmware/flash.sh                 # 之後只更新相簿
./firmware/flash.sh video_player    # 換成影片播放器
```

腳本會把整份讀回來自己比對，並指出不符的位元組落在哪裡 ——
**不要相信 `STM32_Programmer_CLI -v` 的 Data mismatch**，那可能是假警報
（board-notes 10.5）。

## 新板子的硬體前提

- **BOOT0 滑動開關在 SW1 位置**
- Option Bytes：`XSPI1_HSLV=1`、`XSPI2_HSLV=1`、`VDDIO_HSLV=0`
  （出廠通常已正確，用 `STM32_Programmer_CLI -c port=SWD -ob displ` 確認）

## 內容不在這裡的東西

- **照片**：放進 SD 卡（FAT32），相簿會遞迴掃描
- **最愛清單** `我的最愛.txt`：不用自己準備，第一次開機時韌體會在卡的
  根目錄建一個空的（64KB）。純文字，電腦上打得開
- **影片**：`video.bin` 放 SD 卡根目錄，用 `projects/video/tools/mp4pack.py`
  從 mp4 轉出來。太大（動輒上百 MB）不進版控
- **中文字型** `font_zh.c`：6.4MB 的產生物，`scripts/setup.sh` 會現場產生

## 這些檔案裡編進去了什麼

**PSRAM 降到 100MHz**（`shared/xspi_psram.c`）。這是**板子個體差異的補償** ——
原本這塊板子在 BSP 預設的 200MHz 下讀取會出錯（board-notes 10）。

新板子如果撐得住 200MHz，格率可以從 30 翻到 60 fps（board-notes 19 有實測），
但**先別急著改**：

1. 先用這份（100MHz）確認一切正常
2. 想試 200MHz，把 `shared/xspi_psram.c` 那行 prescaler 改回 `Init->ClockPrescaler`
   重新建置 —— 撐不住的症狀是**畫面出現閃爍的細線**，即使畫面靜止也照閃
3. 要確定的話照 board-notes 10.4 的方法掃描整片 PSRAM：
   測試區設成 non-cacheable，並且**一定要有內部 SRAM 的對照組**

錯誤如果是**均勻散布**就是訊號品質（換晶片未必有用，跟 PCB 走線與供電
都有關）；**集中在特定區塊**才是實體壞格，挪開緩衝區就能繞過。

## 更新這些檔案

```bash
cd projects/album && ./scripts/build.sh
arm-none-eabi-objcopy -O binary \
    <cube>/Projects/STM32H7S78-DK/Templates/Album/STM32CubeIDE/Appli/Debug/Album_Appli.elf \
    firmware/album.bin
```

**只在有意義的版本才更新**，不要每次建置都提交 —— `album.bin` 有 1.5MB，
每次都塞進版控會讓倉庫迅速膨脹。
