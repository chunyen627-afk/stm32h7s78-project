# STM32H7S78-DK MJPEG 影片播放

**狀態：可以播。** 影格來源支援 SD 卡與外部 Flash 兩條路，開機自動判斷。
實測 800x480 @ 24fps、0 解碼失敗、0 個跟不上的影格。

## 為什麼是 MJPEG（不是 H.264）

這顆的編解碼週邊只有 **JPEG**（`dcmipp` 是相機輸入、`gpu2d`/`dma2d`/`ltdc` 是繪圖），
沒有任何 H.264/H.265 解碼器，M7 也沒有 NEON，純軟體解 800x480 不可能即時。

更關鍵的是**換格式一點好處都沒有**：實測每格 30.8ms 裡解碼只佔 2.5ms（8%），
轉色 19.9ms 與 PSRAM 頻寬才是瓶頸。MJPEG 的硬體解碼已經近乎免費。

理論上還有「完全不解碼」的路（直接存 RGB565，省掉解碼＋轉色共 22.5ms），
但每格 768KB、五分鐘要 5.5GB，而且 SD 要持續 18MB/s 跟 LTDC 搶 PSRAM，不划算。

## 影格來源

開機先試 SD，失敗就退回 Flash。兩條都保留的理由跟相簿「DMA 失敗退回輪詢」
一樣：讓一條路壞掉變成「換一條」而不是「不能用」。

| 來源 | 適合 | 為什麼 |
|---|---|---|
| SD 卡 `0:/video.bin` | 長片 | 128MB 的外部 Flash 放不下五分鐘的 MJPEG（實測 122MB 已經逼近） |
| 外部 Flash `0x71000000` | 短片 | 記憶體映射，可以就地解碼、零複製，不依賴 SD 卡 |

## 檔案格式（`VFR2`）

**`video.bin` 不是影片檔，播放器打不開。** 它是給韌體讀的封包：

```
magic 'VFR2' │ count │ width │ height │ fps_x100 │ max_size │ 位移表 │ 裸 JPEG × N
```

板子上沒有解多工器，靠位移表直接跳到第 N 格讀出來餵硬體解碼器，
不必解析 MP4 容器。每格補齊到 4 位元組邊界 —— **這不是美觀考量**，
輸入 DMA 的資料寬度是 word，長度不是 4 的倍數硬體會報 USE（board-notes 16.12）。

## 轉檔

```bash
python tools/mp4pack.py <輸入.mp4> <輸出.bin> [--fps 24] [--quality 5] \
                        [--rotate auto|cw|ccw|none] [--start S] [--duration S]
```

- **直式影片會自動旋轉**。720x1280 不轉的話塞進 800x480 只能縮成 270x480，
  兩側全黑（只用到 34% 的寬度）；轉 90 度可以填滿，代價是板子要側著看。
- 等比縮放**不裁切**，置中補黑邊（跟相簿「照片永不裁切」同一個原則）。
- 輸出 `yuvj420p`：每像素 1.5 bytes，YCbCr 中間層只有 4:4:4 的一半，
  板子上的轉色時間直接砍半（board-notes 16.9）。

實測 WBC211221.mp4（720x1280、5 分鐘）→ 7199 格、每格 17.7KB、**121.9 MB**，
轉檔 13 秒。

## 燒錄

```bash
scripts/build.sh                          # 編譯
scripts/flash.sh                          # 韌體進外部 Flash
scripts/flash.sh --frames out.bin         # 影格包進 0x71000000（短片）
scripts/sdwrite.sh out.bin                # 透過 ST-LINK 寫進 SD 卡（長片）
scripts/status.sh                         # 讀出板子狀態
```

`sdwrite.sh` 的原理是讓板子當中介：SWD 把資料塞進 PSRAM，韌體收一塊寫一塊。
實測 SWD 在 24MHz（ST-LINK V3 上限）約 **895 KB/s**（8MHz 是 536 KB/s）。

**目前這條路在「覆寫已存在的 video.bin」時會失敗**，見下方「已知問題」。
沒有讀卡機又碰到這個問題時，就把轉好的 .bin 複製到 SD 卡根目錄手動放進去。

### 燒錄模式的安全設計

使用者的 SD 卡裡有相簿的照片，所以：

1. **要由外部 magic 觸發**（SWD 寫 `g_sdw_go` = `'SDWR'`）。不觸發就完全不會進去，
   播放路徑一行寫入的程式碼都不會執行（board-notes 16.3）。
2. **只碰一個檔案**。檔名寫死，沒有 `f_mkfs`、沒有 `f_unlink`、沒有目錄走訪。
3. **磁碟層預設唯讀**。沒解鎖就掛成 `STA_PROTECT`，FatFs 自己會擋掉寫入。

## 實測數字

外部 Flash 來源、1440 格、素材 24fps：

| 階段 | 每格 |
|---|---|
| 讀檔（Flash 是記憶體映射，零成本） | 0.00 ms |
| JPEG 解碼（DMA） | 2.76 ms |
| YCbCr → RGB565 轉色（DMA2D） | 21.95 ms |
| 換頁＋等垂直消隱＋對時 | 9.21 ms |
| **實測格率** | **24.00 fps**（`late = 0`） |

SD 卡來源多一個讀檔階段，實測 **3.15 ms/格**，比預期少很多。

24 fps 是**素材格率**不是能力上限；扣掉對時之後約 29.5 fps。
轉色換算 64 MB/s，而 LTDC 同時持續佔用 46 MB/s ——
**瓶頸是 100MHz PSRAM 的頻寬，不是運算**（board-notes 16.11）。

## 除錯

板子沒有 UART，狀態一律用全域變數加 SWD 讀（`mode=HOTPLUG`，不要用 UR）。
`scripts/status.sh` 會自動查位址並算成人看得懂的數字。

| 變數 | 意義 |
|---|---|
| `g_dbg_src` | 0=沒來源 1=SD 2=Flash |
| `g_dbg_failmap[4]` / `g_dbg_okmap[4]` | **每格一個位元**（前 128 格），直接看出哪些索引在失敗 |
| `g_dbg_snap[24]` | 凍結在**第一次**解碼失敗當下的 HAL 與 DMA 通道設定 |
| `g_sd_werr` / `g_sd_wsector` / `g_sd_halerr` / `g_sd_halstate` | SD 寫入失敗的現場 |
| `g_sdw_state` | 燒錄模式走到哪（10=就緒 91=掛載失敗） |

`failmap`／`okmap` 這個位元圖是找出 26% 解碼失敗的關鍵：它一次就證明了
失敗是確定性的（兩張圖完全互斥），並指出是哪 32 格 —— 見 board-notes 16.12。

## SD 覆寫踩過的坑（已解）

**不要用 `f_open(FA_CREATE_ALWAYS)` 覆寫大檔。** 它會先把舊檔的整條簇鏈釋放
掉，等於連續打出上百次單磁區的 FAT 寫入，約 130 次之後卡片就不再回應指令
（`HAL_SD_GetError = CMD_RSP_TIMEOUT`）。

正式路徑改成**原地覆寫**：`FA_OPEN_ALWAYS` 保留簇鏈、從位移 0 蓋過去，
新檔比舊檔小才在關檔前 `f_truncate`。實測 122MB 覆寫 192 秒完成。
細節與已排除的假設見 board-notes 17.7。

## 待辦

1. 疊加層／進度條：目前 sdwrite.sh 只有文字進度，板子上沒有顯示
2. YCbCr 中間緩衝區（576KB）搬進內部 SRAM，減少 PSRAM 往返
3. `decode_frame()` 尾端那個多餘的 `HAL_JPEG_Abort()` 可以拿掉
   （真正的解法是 DeInit+Init），但要單獨改、單獨量
4. 沒有音訊。板子有音訊編解碼器（SAI），但目前完全沒做
