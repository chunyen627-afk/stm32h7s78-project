# STM32H7S78-DK 電子相簿

讀 SD 卡上的照片，隨機輪播。全繁體中文介面，觸控操作。

## 功能

- **勾選最上層資料夾**播放（最多 64 個），底下**所有子層**的照片都會撈進來，
  上限 8,192 張、單張 3.4MP，JPEG baseline 4:4:4 / 4:2:2 / 4:2:0
- **隨機播放**（每輪重洗），間隔 **2~5 秒**可調（預設 2 秒）；解碼與顯示重疊，
  只要解碼快過間隔，設定值就是實際展示時間（見下方「效能」）
- **記憶卡熱插拔**、掃描進度顯示、看門狗（16 秒）防卡死
- 對記憶卡**只讀不寫**，磁碟層直接回報寫入保護

### 操作

| 操作 | 效果 |
|---|---|
| USER 短按（<0.6 秒）| 背光 100 → 50 → 20 → 6 → 0% 循環（0% 同時暫停）|
| USER 長按（≥0.6 秒）| 暫停 / 繼續，中央閃 ⏸ ▶ 一秒 |
| 暫停時左滑 | 下一張（維持暫停，可連續翻）|
| 暫停時右滑 | 上一張 |
| 暫停時下滑 | 回選單 |

這塊板子只有一顆 USER 鍵，所以翻頁走觸控手勢 —— 判定區是整個螢幕，
比小按鈕好按，也不用記組合技。按鍵只做兩段（短/長），因為用時間長度
區分三段以上對手指來說太難拿捏。

## 硬體需求

- STM32H7S78-DK 開發板
- microSD 卡，**FAT32**，照片放資料夾裡（支援 `.jpg` / `.jpeg`，baseline）
- USB-C 資料線（純充電線無法辨識）

## 建置與燒錄

需要 STM32CubeIDE（內含 arm-none-eabi-gcc 與 STM32_Programmer_CLI），
以及 Python 3 + Pillow（產生字型用）。

韌體包（STM32CubeH7RS）沿用隔壁 `stm32h7s78-tetris` 專案那份，不重複下載
540MB。放在別處的話設 `CUBE_DIR` 環境變數。

```bash
./scripts/setup.sh          # 建立 CubeIDE 專案（只需一次）
./scripts/build.sh          # 編譯
./scripts/flash.sh --boot   # 第一次燒錄，含 bootloader
./scripts/flash.sh          # 之後只更新應用程式
```

`scripts/` 裡的 `IDE` 與 `CLI` 路徑是寫死的，CubeIDE 裝在別處要改。

## 影像管線

```
SD 讀檔 → 硬體 JPEG 解碼 → YCbCr 轉 RGB888
        → 面積平均縮放（線性光空間）→ 非銳化遮罩 → 有序抖動轉 RGB565 → 旋轉
```

每一步的理由寫在 `app_src/photo.c` 的檔頭。重點：

- **解碼成 RGB888 而不是面板原生的 RGB565**。一開始就砍成 5/6/5，後面所有
  處理都在被量化過的資料上做，誤差會累積。
- **面積平均而不是最近鄰**。1200×1800 縮到 480×800 是 2.25 倍，最近鄰等於
  每 5 個像素丟掉 4 個。
- **盒子邊界要用分數覆蓋率**。整數邊界（來源像素非全進即全出）在 2.25 倍時
  盒子大小會以 2,2,2,3 的週期跳動，邊緣每四個目的像素錯開一次 —— 規律的
  階梯正是「鋸齒」的來源。
- **平均要在線性光空間做**。加權平均只在線性光才有物理意義；sRGB 是感知編碼，
  直接對編碼值平均的誤差集中在邊緣。**這兩件事是相乘的**：單獨做任一項只改善
  一成多，兩項一起改善六成五。
- **縮小後補銳化**。任何正確的縮小都會讓影像變軟，這是物理必然。
  但銳化**不是**抗鋸齒的手段，別拿它當解法。
- **最後才量化並加抖動**。RGB565 的紅藍只有 32 階，平滑漸層會有色帶。
- **旋轉折進最後一次寫入**。面板 800×480 橫式、照片直式，轉成直立才能填滿。

### 效能

`每張 ≈ 647 ms × 來源百萬像素 + 327 ms`（常數項是銳化＋抖動＋旋轉，
它做的是目的地，不隨來源尺寸變）。

展示時間是 `max(間隔設定, 解碼時間)`，所以**超過約 2.6 Mpx 的照片在間隔設
2 秒時會拖到 2.5 秒左右**。不影響操作，但節奏會不一致；在意的話把間隔調成 3 秒。

`app_src/photo.c` 有一整組 `g_dbg_us_*` / `g_dbg_sum_*` 分段計時（微秒，DWT
週期計數器），以及累計來源像素數。**隨機播放每次抽到的尺寸分布都不同，
比較兩次執行一定要用「每百萬像素幾毫秒」，不能直接比平均毫秒數。**

### 畫質測試台

`test/` 可以在 PC 上跑「解碼之後」的整段管線並輸出 PNG。畫質只能用眼睛判斷，
在板子上改一次要重編、重燒、還得手動操作選單。

```bash
cd test && ./run.sh              # 內建測試圖樣
cd test && ./run.sh photo.jpg    # 另外加上真實照片
python reference.py out          # 產生 PIL 的對照組（含線性光 Lanczos）
python compare.py out rings 120 250 90 70 6 <變體...>   # 並排＋數值差異
```

**它直接編譯 `app_src/photo.c` 本體**，不是重寫一份演算法 —— 重寫的話比較的
就不是板子上實際跑的東西。`run.sh` 的 `VARIANTS` 每組一個編譯旗標，
可以一次比較 `AREA_FRACTIONAL`、`RESAMPLE_LINEAR`、`SHARPEN_AMOUNT` 的各種組合。

## 這塊板子的兩個硬體問題

**PSRAM 撐不住 BSP 預設的 200MHz**。時序餘裕有板子個體差異，這塊落在錯誤側，
會偶發單一位元讀取錯誤 —— framebuffer、播放清單、解碼緩衝區全在 PSRAM 裡，
症狀是畫面閃爍、照片路徑字串莫名其妙壞掉（`FR_NO_PATH`）。

`app_src/xspi_psram.c` 覆寫 BSP 的 `__weak MX_XSPI_RAM_Init()` 把時脈減半成
100MHz。實測掃描 16MB：200MHz 有一百多萬個字出錯、0 個乾淨區塊；100MHz 是
零錯誤、256/256 全乾淨。**沒有效能代價**，LTDC 只需要約 46 MB/s。

**外部 NOR Flash 在 `0x70170466` 有一個不穩定位元**。每次燒錄都會出現，目前
落在 FatFs 的查表資料裡，無害。`scripts/flash.sh` 改成整份讀回自己比對，並用
`nm -S` 指出不符的位元組落在哪個符號 —— 落在查表資料可以忽略，落在 `.text`
的函式就不行。不要只看 `STM32_Programmer_CLI -v`，它只丟一行就中止，
判斷不了嚴重性，而且偶爾會誤報。

## 幾個踩過的坑

**FatFs 預設值不能直接用**（`tools/patch_project.py` 會改）：
- `FF_USE_LFN=0` → 長檔名關閉，`IMG_20240101_120000.jpg` 會變 `IMG_20~1.JPG`
- `FF_CODE_PAGE=932`（日文）→ 要改 950（繁中）
- `FF_FS_LOCK=2` → 同時只能開 2 個檔案或目錄，遞迴掃到第三層再開檔就會拿到
  `FR_TOO_MANY_OPEN_FILES(18)`

**不使用 ST 的 `fatfs.c`**。它的 `MX_FATFS_Process()` 含 `f_mkfs`（格式化）
與寫測試檔的路徑。相簿只讀不寫，掛載那幾行自己寫，把誤格式化的可能性從根本
移除。磁碟介面在 `core/sd_bsp_diskio.c`，回報 `STA_PROTECT`。

**`HAL_JPEG_Decode()` 返回後讀不到輸出長度**。HAL 在結束時會先用
`DataReadyCallback` 把長度交出來，然後把 `JpegOutCount` 清成 0。`__weak` 的
預設回呼什麼都不做，長度就這樣掉了，轉色函式收到 `DataCount=0` 一個 MCU 都
不會轉，整片緩衝區是雜訊。必須自己實作回呼把長度接住。

**`jpeg_utils` 的 `JPEG_RED_OFFSET` 是位元位移，不是位元組索引**。RGB888 預設
`RED_OFFSET=16`，記憶體順序是 B,G,R。設 `JPEG_SWAP_RB=1` 才是直覺的 R,G,B。

**`BSP_SD_Init()` 可能永遠不返回**。HAL 的 `SD_InitCard()` 裡有
`while (sd_rca == 0U)`，沒有逾時保護。卡片若停在異常狀態不回應 CMD3 就出不來
（實測畫面停在「載入中」，CPU 正常在跑、故障暫存器全 0，就是空轉）。程式自己
跳不出無窮迴圈，只能靠獨立看門狗（16 秒）兜底；重置後讀 `RCC_FLAG_IWDGRST`
判斷是被看門狗打掉的，先擋住不要再碰卡片，並提示使用者拔卡重插。

軟體重置**不會**切斷 SD 卡電源，所以在讀取途中重置板子容易把卡片留在異常
狀態。除錯時要注意。

**`ChromaSubsampling = 0` 是 4:4:4，不是 4:2:0**。緩衝區照 4:2:0 配的話，
4:4:4 的照片（每像素 3 bytes，兩倍空間）裝不下，而 HAL 在輸出緩衝區滿時
**繞回開頭續寫** —— 症狀正是「照片底部跑到最上面、底部殘留舊資料」。
輸出緩衝區要照 4:4:4 最壞情況配，容量檢查照實際取樣格式算。

**`DataReadyCallback` 裡不要呼叫 `HAL_JPEG_ConfigOutputBuffer`**。解碼結束
時 HAL 也會呼叫回呼，這時再餵新緩衝區會反覆觸發，把整塊緩衝區吃光 ——
輸出長度「精確等於緩衝區容量」就是這個訊號。回呼只記錄長度（取結束位移
的最大值，不要累加）。

**縮放目的地尺寸不要用比例反算**。定點數兩次截斷會讓 800 變 799，最後
一列永遠沒畫到。填滿模式的目的地直接寫死成整個螢幕，讓誤差落在來源取樣
座標上。

**`BSP_LCD_DisplayOn` 不會開背光**（`DisplayOff` 會順手關掉），要自己把
GPIOG15 拉高。**`BSP_SD_IsDetected` 只讀不設定偵測腳**，第一次
`BSP_SD_Init` 之前要自己把 `SD_DETECT_PIN` 設成輸入，否則讀到浮接電位。

**觸控座標要在確認的當下抓**。確認完再讀一次，輕點時手指已放開會撲空，
按鈕就像沒反應。等待放開要連續數次都讀不到才算（放開瞬間會彈跳）。

更完整的版本（含 LTDC、DMA2D、PSRAM、建置系統）在
[docs/board-notes.md](../../docs/board-notes.md)。

## 專案結構

```
app_src/     韌體整合
  album_main.c   主流程：掛載、掃描、選單、播放、待機、熱插拔、MPU 快取政策
  photo.c        影像管線：讀檔、解碼、縮放、銳化、抖動、旋轉
core/        可移植的部分
  sd_bsp_diskio.c  FatFs 磁碟介面，接 BSP_SD，唯讀
  font_zh.c        中文點陣字（tools/genfont.py 產生）
  jpeg_utils_conf.h
test/        PC 端畫質測試台（直接編譯 app_src/photo.c）
  resample_shot.c  驅動程式，輸出 framebuffer 原始檔
  stubs/           HAL / FatFs / jpeg_utils 的替身，只為了編得過
  run.sh           編譯各變體並跑完整批次
  make_testimg.py  測試圖樣（放射狀、多角度硬邊、同心圓）
  reference.py     PIL 對照組（sRGB / 線性光 × BOX / Lanczos）
  compare.py       並排圖＋平均絕對差
  fb2png.py        framebuffer 轉直式 PNG
  zoom.py          裁切放大到像素等級
scripts/     setup / build / flash
tools/       genfont.py（字型產生）、patch_project.py（改 CubeIDE 專案檔）
```

共用程式在 repo 根目錄的 `shared/`：`gfx.c`（直立繪圖層，沿用 tetris 專案）、
`xspi_psram.c`（PSRAM 降頻，板子個體差異的補償）。`scripts/build.sh` 會把
兩邊同步進 CubeIDE 專案。
