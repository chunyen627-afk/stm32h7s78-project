# STM32H7S78-DK 電子相簿

讀 SD 卡上的照片，隨機輪播。全繁體中文介面，觸控操作。

## 功能

- **勾選最上層資料夾**播放，選到的資料夾底下**所有子層**的照片都會撈進來
- **隨機播放**（Fisher-Yates 洗牌，每輪重洗），間隔 **2～5 秒**可調
- **USER 鍵當待機鍵**：按一下關螢幕並暫停，再按一下接著播
- **記憶卡熱插拔**：拔卡自動卸載並提示，插回自動重新掃描
- 對記憶卡**只讀不寫**，磁碟層直接回報寫入保護

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
        → 盒狀濾波縮放（8 位元精度）→ 非銳化遮罩 → 有序抖動轉 RGB565 → 旋轉
```

每一步的理由寫在 `app_src/photo.c` 的檔頭。重點：

- **解碼成 RGB888 而不是面板原生的 RGB565**。一開始就砍成 5/6/5，後面所有
  處理都在被量化過的資料上做，誤差會累積。
- **盒狀濾波而不是最近鄰**。1200×1800 縮到 480×800 是 2.25 倍，最近鄰等於
  每 5 個像素丟掉 4 個。
- **縮小後補銳化**。任何正確的縮小都會讓影像變軟，這是物理必然。
- **最後才量化並加抖動**。RGB565 的紅藍只有 32 階，平滑漸層會有色帶。
- **旋轉折進最後一次寫入**。面板 800×480 橫式、照片直式，轉成直立才能填滿。

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

## 專案結構

```
app_src/     韌體整合
  album_main.c   主流程：掛載、掃描、選單、播放、待機、熱插拔
  photo.c        影像管線：讀檔、解碼、縮放、銳化、抖動、旋轉
  xspi_psram.c   PSRAM 降頻（板子個體差異的補償）
core/        可移植的部分
  gfx.c          直立繪圖層（沿用 tetris 專案）
  sd_bsp_diskio.c  FatFs 磁碟介面，接 BSP_SD，唯讀
  jpeg_utils_conf.h
scripts/     setup / build / flash
tools/       genfont.py（字型產生）、patch_project.py（改 CubeIDE 專案檔）
```
