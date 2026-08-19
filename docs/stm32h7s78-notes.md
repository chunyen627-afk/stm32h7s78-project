# STM32H7S78-DK 開發筆記

這份記錄的是**做這塊板子時實際踩到的坑**，以及花時間才查出來的正確做法。
之後在這塊板子上做別的專案，先讀這份可以省下大量除錯時間。

每一條都標註了「症狀 → 原因 → 解法」，因為症狀往往跟原因看起來毫無關聯。

---

## 板子基本規格

| 項目 | 規格 |
|---|---|
| MCU | STM32H7S7L8H6H，Cortex-M7 @ 600 MHz |
| **內部 Flash** | **只有 64 KB** ← 這點決定了整個架構 |
| 外部 Flash | 128 MB Octo-SPI NOR（Macronix MX66UW1G45G），映射在 `0x70000000` |
| 外部 RAM | 32 MB PSRAM（AP Memory APS256XX），映射在 `0x90000000` |
| LCD | 5 吋 800×480 RGB565，RK050HR18 面板（MB1860 子板） |
| 觸控 | GT911 電容式五點觸控，I2C1，位址 `0xBA` |
| 除錯器 | 板載 ST-Link V3（`VID_0483 / PID_3754`） |

**內部 Flash 只有 64 KB** 是這塊板子最特別的地方。一個 800×480 RGB565 的
framebuffer 就要 750 KB，程式碼根本放不進內部 Flash。所以必須用 XIP 架構：
內部 Flash 放 bootloader，程式本體燒進外部 Flash 直接執行。

---

## 一、專案建置

### 1.1 必須用 Template_XIP 當基底

ST 韌體包裡 `Projects/STM32H7S78-DK/Templates/Template_XIP` 已經處理好
bootloader、XIP 鏈結腳本、XSPI 記憶體映射。不要從零開始搭。

### 1.2 專案目錄不能隨便搬

**症狀**：編譯報 `No rule to make target 'C:/Middlewares/ST/...'`

**原因**：`.cproject` 用相對路徑 `../../../../../../..` 指向韌體包的
`Drivers/` 和 `Middlewares/`。把專案複製到別的地方，往上爬的層數就錯了，
會爬到磁碟根目錄。

**解法**：專案必須放在 `cube/Projects/STM32H7S78-DK/Templates/<你的專案>`
這個層級。

### 1.3 submodule 一定要抓

**症狀**：缺 `stm32_boot_xip.c`、`stm32h7rsxx_hal_ltdc.c` 等檔案

**原因**：`STM32CubeH7RS` 倉庫把 HAL、CMSIS、BSP、GT911 都做成 submodule，
`git clone --depth 1` 不會帶下來。

**解法**：
```bash
git submodule update --init --depth 1 --recursive
```

### 1.4 元件設定檔要自己補

Template_XIP 不含 `gt911_conf.h`、`mx66uw1g45g_conf.h`、`aps256xx_conf.h`，
但 BSP 需要它們。從 `Projects/STM32H7S78-DK/Examples/BSP/Inc/` 複製過來。

### 1.5 HAL 模組預設全是關的

`stm32h7rsxx_hal_conf.h` 裡 LTDC / I2C / DMA2D / XSPI / LPTIM 預設都被註解掉。
少開一個就是一串 `unknown type name` 錯誤。

LCD 背光用 LPTIM，所以連 LPTIM 都要開——這個很容易漏。

### 1.6 命令列編譯

CubeIDE 可以不開 GUI 編譯，但有兩個雷：

```bash
# 路徑必須用 Windows 反斜線格式，否則 Eclipse 會把 C: 當成 URI scheme
# 報錯 "No file system is defined for scheme: C"
stm32cubeidec.exe -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data 'C:\path\to\ws' -import 'C:\path\to\project' \
  -build 'ProjectName/Debug'
```

`-import` 和 `-build` 要在**同一次呼叫**裡，分兩次執行工作區狀態不會保留。

### 1.7 燒錄

```bash
# Bootloader 進內部 Flash（只需第一次）
STM32_Programmer_CLI -c port=SWD mode=UR -w Binary/Boot_XIP.hex -v

# 應用程式進外部 Flash，必須指定 external loader
STM32_Programmer_CLI -c port=SWD mode=UR \
  -el ".../ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr" \
  -w Appli/Debug/App.elf -v
```

漏掉 `-el` 就燒不進外部 Flash。

**硬體前提**：BOOT0 滑動開關要在 SW1 位置。Option Bytes 需要
`XSPI1_HSLV=1`、`XSPI2_HSLV=1`、`VDDIO_HSLV=0`（出廠通常已正確，可用
`-ob displ` 確認）。

---

## 二、顯示（LTDC）── 這裡的坑最多

### 2.1 BSP_LCD_Init 預設是 ARGB8888，不是 RGB565

**症狀**：畫面左右重複兩次、下半部是彩色雜訊、顏色完全不對

**原因**：`BSP_LCD_Init()` 內部呼叫
`BSP_LCD_InitEx(..., LCD_PIXEL_FORMAT_RGB888, ...)`，而 BSP 把 RGB888 實作成
**ARGB8888（4 bytes/pixel）**。如果你的繪圖是 RGB565（2 bytes/pixel），
硬體讀取的行距就是你的兩倍，畫面自然錯亂。

**解法**：明確指定像素格式。
```c
BSP_LCD_InitEx(0, LCD_ORIENTATION_LANDSCAPE,
               LCD_PIXEL_FORMAT_RGB565, 800, 480);
```

### 2.2 換 framebuffer 千萬不要用 BSP_LCD_SetLayerAddress

**症狀**：畫面持續閃爍、出現橫向黑線

**原因**：`BSP_LCD_SetLayerAddress()` → `HAL_LTDC_SetAddress()` →
`LTDC_SetConfig()`，而 `LTDC_SetConfig` 為了換一個位址，**重寫整組圖層暫存器**：
視窗座標、像素格式、混色係數、行長、行數……而且好幾個是這種寫法：

```c
LTDC_LAYER(...)->CFBLR &= ~(mask);   /* 先清成 0 */
LTDC_LAYER(...)->CFBLR  = (value);   /* 再寫回 */
```

兩步之間暫存器是 0。面板持續掃描，掃到那一瞬間就是黑線。

**解法**：只寫 `CFBAR` 一個暫存器。它是 shadow register，寫入不會立即生效，
等垂直消隱重載時整批原子切換。

```c
LTDC_Layer1->CFBAR = new_framebuffer_addr;   /* 單次寫入，無中間狀態 */
LTDC->SRCR = LTDC_SRCR_VBR;                  /* 垂直消隱時生效 */
while (LTDC->SRCR & LTDC_SRCR_VBR) { }       /* 等它真的切過去 */
```

**這是本專案最重要的一條**，任何用 LTDC 做雙緩衝的專案都適用。

初始化時記得先讓 BSP 進入 no-reload 模式，否則 `SetLayerAddress` 仍會走
立即重載那條路：
```c
BSP_LCD_Reload(0, BSP_LCD_RELOAD_NONE);
```

### 2.3 雙緩衝的索引很容易寫反

**症狀**：畫面閃爍，但暫停（畫面靜止）時不閃

**原因**：交換後把繪圖目標指到「正在顯示」的那塊緩衝區，等於雙緩衝失效，
每格都在使用者眼前重畫。

**判斷技巧**：如果**畫面靜止時不閃、有動畫時才閃**，幾乎可以確定是這個問題，
而不是時序或硬體問題。

**解法**：用紙筆或小程式把狀態機跑幾格，確認「顯示的」和「繪製的」永遠相反。

### 2.4 PSRAM 要設成 write-through

Framebuffer 放在 PSRAM，CPU 寫入會停在 D-Cache，而 LTDC 是直接從 PSRAM 讀，
兩邊看到的內容會不一致。

```c
MPU_Region_InitTypeDef mpu = {0};
mpu.BaseAddress  = 0x90000000;
mpu.Size         = MPU_REGION_SIZE_4MB;
mpu.IsCacheable  = MPU_ACCESS_CACHEABLE;
mpu.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;   /* 這組合 = write-through */
mpu.TypeExtField = MPU_TEX_LEVEL0;
```

註：實測這項對**速度**沒有明顯影響，但對**正確性**是必要的。

### 2.5 PSRAM 開機是隨機內容

一定要在顯示開啟前把兩塊 framebuffer 清空，否則第一幀會看到雜訊。

### 2.6 直立顯示要自己做旋轉

BSP 只支援 `LCD_ORIENTATION_LANDSCAPE`，沒有 portrait 選項。
要做直立畫面就得在繪圖層自己轉座標：

```c
/* 邏輯直立 (x,y) -> 實體橫向 (y, GFX_W-1-x) */
offset = (GFX_W - 1 - x) * PHYS_W + y;
```

觸控讀進來要做**反向**轉換，兩者必須互為逆運算，否則手指按的位置和畫面上
按鍵的位置對不上。這點務必寫測試驗證。

---

## 三、DMA2D ── 已解決

### 3.1 根本原因：CPU 讀取時的快取一致性

**症狀**：畫面出現大範圍白色雜點（偶發、一整片同時出現），控制區出現遊戲
場地的顏色碎片。

**原因**：DMA2D 繞過 CPU 直接寫 PSRAM，但 **CPU 讀取時仍會從 D-Cache 取值**，
拿到的是 DMA2D 寫入之前的舊內容。

這裡最容易誤判：framebuffer 已經設成 **write-through**，所以我一開始排除了
快取的可能。但 write-through 只保證「CPU 的**寫入**」會穿透到 PSRAM，
**完全管不到「CPU 的讀取」這個方向**。DMA 寫、CPU 讀，仍然需要手動失效快取。

**解法**：讀取被 DMA2D 寫過的區域之前，先失效該範圍的快取。

```c
SCB_InvalidateDCache_by_Addr((uint32_t *)addr, (int32_t)byte_len);
```

### 3.2 診斷方法（值得複製到其他專案）

雜點是偶發的，在遊戲裡很難定位。做法是把 DMA2D 單獨拉出來寫成診斷程式
（`app_src/dma2d_probe.c`），用固定樣式反覆填色再逐一驗證，把「偶發」變成
「可量測的錯誤率」，並在出錯當下記錄硬體暫存器狀態。

四個階段，逐步加壓：
1. 單次大面積填色 ×200
2. 200 格小矩形 ×100 回合（模擬遊戲用法）
3. DMA2D 與 CPU 交錯寫入相鄰區域 ×200
4. 連續發送不逐次等待 ×100

結果寫進全域變數，用 SWD 讀出來，不需要 UART：
```bash
addr=$(arm-none-eabi-nm app.elf | grep " probe_errors$" | awk '{print "0x"$1}')
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 $addr 1
```

**關鍵線索**：錯誤像素的實際值是「上一回合的顏色」而不是隨機值 → 說明不是
寫錯資料，是**讀到了舊內容**。同時硬體狀態顯示 TC 已置位、START=0、傳輸次數
完全正確 → 傳輸本身沒問題。兩者一對照，就只剩快取這一個可能。

### 3.3 其他必須做對的事

**不要用 HAL_DMA2D_Start + HAL_DMA2D_PollForTransfer**：
`PollForTransfer` 的等待迴圈包在 `if (CR & DMA2D_CR_START)` 裡。小矩形傳輸
很快，常在程式走到 poll 之前就結束，START 已被硬體清 0，整個等待被跳過。
而 `HAL_DMA2D_Start` 又從不清除 TC 旗標，殘留的 TC=1 會讓下一次等待立刻返回。

直接操作暫存器比較可靠（見 `app_src/dma2d.c`）：
```c
DMA2D->OCOLR = color;        /* R2M + RGB565 輸出時直接放 16 位元值 */
DMA2D->OMAR  = dest_addr;
DMA2D->OOR   = stride - width;
DMA2D->NLR   = (width << DMA2D_NLR_PL_Pos) | height;
DMA2D->IFCR  = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;
DMA2D->CR   |= DMA2D_CR_START;

while (DMA2D->CR & DMA2D_CR_START) { }   /* 看 START 位，不要看旗標 */
__DSB();
```

**注意 OCOLR 的格式取決於你怎麼呼叫**：
- 直接寫暫存器：依輸出格式，RGB565 就放 16 位元值
- 走 `HAL_DMA2D_Start`：**一律要 ARGB8888**，HAL 自己narrow。傳 RGB565
  進去會讓紅色通道整個變 0，畫面發灰

**小矩形用 CPU 反而快**：DMA2D 每次要設四個暫存器再啟動，幾百個像素的矩形
設定成本大於傳輸本身。實測門檻約在 1024 像素。

### 3.4 混用硬體與 CPU 繪圖時必須同步

**症狀**：某個區域的小圖案不完整而且會閃，但其他區域正常。

**原因**：填色是非同步的（發出傳輸後函式就返回）。若隨後用 CPU 在同一區域
畫小東西，DMA2D 正在進行的傳輸會把剛畫好的內容蓋掉。

這個 bug 只在**同一塊區域混用兩種路徑**時出現。俄羅斯方塊的預覽框正是這樣：
框底 190x68 走硬體，裡面的方塊 14x14 走 CPU，於是方塊被框底抹掉。

**解法**：在繪圖層設一個同步點，任何 CPU 寫入 framebuffer 之前先等硬體停下來。

```c
/* gfx.c */
static gfx_sync_hw_fn g_sync_hw;
static inline void sync_hw(void) { if (g_sync_hw) g_sync_hw(); }

/* 韌體初始化時 */
gfx_set_fill_hw(dma2d_fill);
gfx_set_sync_hw(dma2d_wait);
```

**同步點的粒度很重要**：放在 `gfx_pixel` 會讓畫一行字多做上千次檢查。
應該放在 `gfx_char`、`gfx_circle` 這種「一次寫幾百個像素」的函式開頭。

之後改用 GPU2D 也是同一個模式，這個 callback 機制可以直接沿用。

### 3.5 效能

| | 繪製 | 等待垂直消隱 | fps |
|---|---|---|---|
| 純 CPU | 9.87 ms | 5.71 ms | 64 |
| DMA2D（門檻 1024px） | 8.94 ms | 6.63 ms | 64 |

幀率相同，因為瓶頸是面板的 60 Hz。DMA2D 的價值在於**騰出 CPU 時間**，
以及未來做全畫面重繪（賽車類）時才會顯現。

### 3.6 這顆還有更強的 GPU2D

`GPU2D_BASE` 存在，HAL 有 `stm32h7rsxx_hal_gpu2d.c`。那是 NeoChrom GPU，
支援旋轉、縮放、紋理貼圖、抗鋸齒——DMA2D 只能做矩形搬運與混色。
做賽車這類需要旋轉縮放的遊戲時應該用它。

## 四、效能實測數據

用 DWT cycle counter 在板子上實際量測（600 MHz）：

| 項目 | CPU 繪圖 | DMA2D（門檻 1024px） |
|---|---|---|
| 每格繪製 | 9.87 ms | 8.94 ms |
| 等待垂直消隱 | 5.71 ms | 6.63 ms |
| 合計 | 15.58 ms | 15.57 ms |
| 幀率 | 64 fps | 64 fps |

**結論**：兩者幀率相同，因為瓶頸是面板的 60 Hz 更新率，不是繪製速度。
單純的 2D 遊戲用 CPU 畫就夠了；DMA2D 的價值要在全畫面重繪時才顯現。

註：目前遊戲的方塊是 24×24 = 576 像素，低於 1024 的門檻，所以場地其實
還是走 CPU。若把門檻調低反而更慢（設定成本大於傳輸）。真正要發揮 DMA2D，
應該改成「一次傳輸畫一大塊」而不是「每個小方塊一次」。

量測方法：
```c
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CYCCNT = 0;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
uint32_t t0 = DWT->CYCCNT;
/* ... 要量的程式 ... */
uint32_t cycles = DWT->CYCCNT - t0;
```

之後用 `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <變數位址> 1`
就能把數值讀出來，不需要接 UART。變數位址用
`arm-none-eabi-nm app.elf | grep 變數名` 查。

---

## 五、資源用量（決定能塞多少遊戲）

| 項目 | 大小 |
|---|---|
| 遊戲本體（邏輯+繪圖+UI+輸入+字型） | 16.4 KB Flash / 296 B RAM |
| 其中中文字型（74 字，可共用） | 5.6 KB |
| 扣掉字型的純遊戲 | 10.8 KB |
| HAL + BSP 基礎建設（所有遊戲共用） | 約 145 KB |
| Framebuffer ×2（所有遊戲共用） | 1.5 MB PSRAM |

外部 Flash 有 **128 MB**，目前只用了 60 KB。
純程式類的 2D 遊戲每款約 10~20 KB，**容量完全不是限制**。

---

## 六、中文字型

BSP 內建字型只有 ASCII。中文要自己做點陣字：

`tools/genfont.py` 用 Pillow 從微軟正黑體算出點陣圖，產生 C 陣列。
24×24 每字 72 bytes，74 個字只要 5.2 KB。

搭配 `tools/checkglyphs.py` 在建置前檢查所有 UI 字串用到的字都在字型表裡——
**漏字在畫面上是「什麼都不顯示」，不會報錯**，所以這個檢查很有必要。

---

## 七、測試策略

沒有板子也能測大部分東西：用 **QEMU 的 `mps2-an500`**（Cortex-M7，跟這塊板子
同核心），配合 semihosting 輸出。

```bash
qemu-system-arm -M mps2-an500 -nographic \
  -semihosting-config enable=on,target=native -kernel test.elf
```

需要 `-specs=rdimon.specs`，並在啟動碼裡呼叫 `initialise_monitor_handles()`
（用 `-nostartfiles` 時 crt0 不會自動呼叫它，輸出會是空的）。

可以測到的：遊戲邏輯、座標旋轉、字型資料、記憶體越界、觸控映射。
測不到的：真實顏色、畫面方向、觸控準度、閃爍。這些只能燒上板用眼睛看。

**教訓**：純數學測試驗證了「旋轉映射是雙射」，卻沒發現**畫面整個上下顛倒**——
雙射只保證一對一，不保證方向正確。所以務必把畫面**渲染成 PNG 檢查**
（`test/render_shot.c` + `tools/raw2png.py`），視覺問題只有看才看得出來。

---

## 八、通用教訓

1. **症狀的位置就是線索**。「只有 A 鈕那側閃」讓我算出是 6 px 的重繪縫隙；
   「暫停時不閃」直接證明問題在動畫而非硬體時序。使用者的精確描述比我的推理有用。

2. **二分法比推理快**。花了三輪猜 DMA2D 的問題出在哪，最後直接把它關掉，
   一次就確定了範圍。

3. **量測，不要假設**。我先後假設「繪製太慢」「快取沒寫回」，實測後都被推翻。
   DWT cycle counter 花五分鐘裝上去，省下數小時瞎猜。

4. **測試要先驗證它會失敗**。寫完閃爍偵測測試後，我故意把 bug 改回去確認測試
   真的會 FAIL——第一版測試其實是無效的，改回去也照樣 PASS。
