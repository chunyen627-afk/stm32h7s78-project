# STM32H7S78-DK MJPEG 影片播放

**狀態：可以播，解碼失敗已歸零。** 120 格 800x480 的素材連續循環播放，
實測 3277 格 **0 失敗**、0 個跟不上的影格。

## 為什麼有這個專案

相簿那邊留下一個未解問題：JPEG 硬體用 DMA 解碼在隔離測試裡完全正確，
整合進正式路徑卻會在第一張之後把週邊卡死（board-notes 16.6）。
影片是那個問題的極端版本：每秒連續解二十幾次，中間夾著轉色與換頁。
在乾淨的環境裡處理，比在相簿裡邊改邊壞好。

那個問題已經解開（board-notes 16.8：週邊會累積狀態，`HAL_JPEG_Abort()`
清不掉，每格要做完整的 `DeInit + Init`）。

## 設計

- **沒有 SD 卡**：影格打包成二進位燒進外部 Flash 的空白區（0x71000000）。
  外部 Flash 是記憶體映射的，韌體直接就地讀，不必複製也不必解析容器。
- **沒有縮放**：影格預先轉成 800x480，轉色直接寫進 framebuffer。
- **沒有 UI**：不引入繪圖層與中文字型。
- **RGB565 輸出**：轉色結果就是最終畫面，少一個緩衝區與一趟 PSRAM 來回。
- **轉色走 DMA2D**：CPU 轉色要 64ms/格，佔整格的 91%；DMA2D 是 20ms。

## 實測（3277 格平均，素材 24 fps）

| 階段 | 每格 |
|---|---|
| JPEG 解碼（DMA） | 2.58 ms |
| YCbCr -> RGB565 轉色（DMA2D） | 19.93 ms |
| 換頁＋等垂直消隱＋對時 | 8.29 ms |
| 合計 | 40.98 ms → **24.39 fps** |

24.39 fps 是**素材格率的上限**（`SRC_FPS = 24`，每格 41ms），不是能力上限 ——
`g_dbg_late = 0` 代表每一格都在期限內做完，還有餘裕。要量真正的能力上限
必須先把對時關掉，那是另一次測量。

轉色的 19.93 ms 換算是 64 MB/s（讀 576KB + 寫 768KB），而 LTDC 同時持續佔用
46 MB/s —— **瓶頸是 100MHz PSRAM 的頻寬，不是運算**（board-notes 16.11）。

## 除錯

板子沒有 UART，狀態一律用全域變數加 SWD 讀。變數位址用
`arm-none-eabi-nm Video_Appli.elf | grep g_dbg` 查，
用 `mode=HOTPLUG`（不要用 UR，那會把核心 halt 在 reset 點）。

```bash
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x2400020C 232
```

計數器從 `g_dbg_stage` 起是連續配置的，一次讀完 232 bytes 就有全部狀態。
比較有用的幾個：

| 變數 | 意義 |
|---|---|
| `g_dbg_nframe` / `g_dbg_fail` | 成功／失敗的影格數 |
| `g_dbg_failmap[4]` / `g_dbg_okmap[4]` | **每格一個位元**，直接看出是哪些索引在失敗 |
| `g_dbg_snap[24]` | 凍結在**第一次**失敗當下的 HAL 與 DMA 通道設定 |
| `g_dbg_sum_dec/cc/out/all` | 各階段累計微秒，除以 `nframe` 得平均 |
| `g_dbg_late` | 跟不上素材格率的次數 |
| `g_dbg_ltdc_fu` / `g_dbg_ltdc_te` | LTDC FIFO underrun／傳輸錯誤 |

`g_dbg_freeze` 與 `g_dbg_nopresent` 是 SWD 可寫的開關，用來把「顯示」與
「產生內容」分開。

`failmap`／`okmap` 這個位元圖是找出 26% 解碼失敗的關鍵：它一次就證明了
失敗是確定性的（兩張圖完全互斥），並指出是哪 32 格 —— 詳見 board-notes 16.12。

## 影格準備

```bash
ffmpeg -framerate 1 -i src/%03d.jpg \
  -vf "scale=1600:-1,zoompan=...:s=800x480,fps=24" -q:v 5 frames/f%03d.jpg

python tools/packframes.py frames frames.bin   # 120 幀 6.46MB
```

打包時每格會補齊到 4 位元組邊界。**這不是美觀考量**：輸入 DMA 的資料寬度是
word，每一塊的長度都必須是 4 的倍數，補齊讓「往上取整」永遠讀得到有效記憶體
（board-notes 16.12）。

## 待辦

1. 關掉對時，量真正的格率上限
2. YCbCr 中間緩衝區（576KB）搬進內部 SRAM，減少 PSRAM 往返
3. `decode_frame()` 尾端那個多餘的 `HAL_JPEG_Abort()` 可以拿掉
   （真正的解法是 DeInit+Init），但要單獨改、單獨量
