# USB 無線耳機（USB Host Audio）— 音質已可接受，尚未整合

目標：**讓相簿的影片播放走無線耳機，而且 3.5mm 耳機孔要保留**
（使用者 2026-08-26 明確要求：兩條輸出並存，不是二選一）。

這個需求在時脈那一關已經滿足 —— USB 走 HSI48 + CRS(LSE)，相簿的 PLL3
完全不用動。**要是當初選了 PLL3Q = 48MHz 那條路，3.5mm 的 I2S 就會變成
音高差 2.3%，現在就得回頭重做。**這個目錄是通往那個目標的第一步，
狀態是「**可行性已證明、音質使用者已認可、還沒接進相簿**」。

2026-08-26 更新：週期性爆音的根因找到並修好了（ST 範例把 USB 的 48MHz
設成 46.08MHz），使用者實聽後說「音質可以接受」。

## 一句話結論

**這塊板子可以當 USB 主機驅動 2.4GHz 無線耳機的 dongle。**
實測 JBL Quantum TWS（VID `0x0ECB` / PID `0x208A`）列舉成功、
ST 的 UAC1 類別掛得上、從 SD 卡串流 wav、耳機真的出聲。

但 **ST 的範例不足以做產品** —— 三個真 bug，其中 USB 時脈那個是週期性
爆音的根因（見下）。

## 硬體怎麼接（這一段卡最久）

| | |
|---|---|
| 用哪個孔 | **CN17（USB2，Full Speed）** —— 相簿的隨身碟模式用 CN18，兩者不衝突 |
| VBUS 怎麼來 | **JP1 要同時插兩個跳線帽：`STLK` + `USB2`** |

JP1 是 2x4 排針、四欄各一個來源（`USB1` / `USB2` / `5VIN` / `STLK`），
**它是純粹的「電從哪裡來」選擇器**：只插 USB2 的話板子自己沒電（實測螢幕不亮）。
插兩個帽等於把 CN7 的 5V 同時接到板子與 CN17 的 VBUS，dongle 就有電了。

**鐵則：USB2 那欄插著跳線時，CN17 絕對不能再插電腦或充電器** ——
那會變成兩個 5V 電源對接。長期方案應該改成 `5VIN + USB2` 配外接電源，
或用「USB-C OTG 帶供電」的轉接線（host 堆疊的 `vbus_sensing_enable`
是 DISABLE，所以不在乎 5V 從哪來）。

驗證有沒有供電成功：讀 `HPRT`（`0x40080440`）。
`0x00021405` = PCSTS 偵測到裝置、PENA 埠已啟用、PPWR 有供電、PSPD=01 全速。

## 怎麼重建這個治具

ST 的範例在 `cube/Projects/STM32H7S78-DK/Applications/USB_Host/AUDIO_Standalone`，
而 **cube/ 是 gitignore 的**，所以修改保存在這裡：

```bash
python tools/patch_usbaudio.py          # ffconf（exFAT）
# audio.c 的四處修改：patches/audio.c.patched 是改好的完整檔案
```

建置（跟相簿一樣走 headless，用**獨立的 workspace**）：

```bash
IDE=C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/stm32cubeidec.exe
"$IDE" -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild   -data 'C:\STM32CubeIDE\ws_headless_usbhost'   -import '<...>/AUDIO_Standalone/STM32CubeIDE/Appli'   -build 'AUDIO_Standalone_Appli/Debug'
```

Boot 燒內部 Flash、Appli 燒外部 Flash（要 `-el` 外部載入器），跟相簿一樣。
**測完記得把相簿燒回去**（`scripts/flash.sh --boot` 再 `scripts/flash.sh`）——
相簿的 bootloader 有 HSE 的修正，ST 範例那顆沒有。

狀態訊息走 **UART4 = ST-LINK 的虛擬序列埠（COM4，115200 8N1）**。
這是這塊板子少數看得到 printf 的場合。

## ST 範例的三個真 bug（改掉才聽得到正常聲音）

第三個（USB 時脈 46.08MHz）寫在下面獨立的一節，因為它才是爆音的根因。

### 1. WAV 解析寫死第 44 個位元組

`WAV_InfoTypedef` 假設 `data` 緊接在 `fmt` 後面。但 **ffmpeg 會在中間插一個
LIST 區塊** —— 實測我們卡上的檔案 data 在第 **78** 個位元組。

偏移 34 **不是 4 的倍數**，而 16-bit 立體聲一個取樣框是 4 bytes，
所以每個取樣被切成兩半。症狀是「**聽得出原本的聲音，但疊著刺耳雜訊，
而且音量越大越嚴重**」—— 雜訊跟著訊號放大，代表波形本身被弄壞。

我們自己的韌體（`projects/album/app_src/audio_out.c`）是走區塊鏈找 `data` 的，
不會中這一招。轉檔工具 `video2bin.py` 產出的 wav 也一定有 LIST 區塊。

### 2. SetFrequency 的回傳值沒被檢查

`USBH_AUDIO_SetFrequency` 是非同步的控制傳輸，第一次一定回 `USBH_BUSY`。
範例呼叫一次就不管 —— **取樣率從來沒有真的設定到裝置上**。
實測印出來是 `SetFrequency -> 1`（1 = USBH_BUSY）。

改成在播放狀態裡重試到 `USBH_OK`。**不能在原地卡迴圈等**，因為完成那筆
控制傳輸需要主迴圈去跑 `USBH_Process`。

## 爆音的根因：ST 範例把 USB 的 48MHz 設成 46.08MHz

2026-08-26 查到並修好。**這是 ST 範例的第三個真 bug，而且它才是爆音的原因。**

`Boot/Src/main.c` 的 `SystemClock_Config`：

```
HSE 24MHz / PLLM 5 = 4.8MHz -> PLLN 240 -> VCO 1152MHz -> PLLQ 25 = 46.08MHz
```

而 `Appli/Src/usbh_conf.c` 指定 `RCC_USBOTGFSCLKSOURCE_PLL3Q` 當 USB 全速的
48MHz 來源。**46.08 / 48 = 0.96。**

全速匯流排的 SOF 應該是 1000 Hz，實測**只有 960 Hz**。等時端點一個訊框送
一包 192 bytes，所以 dongle 每秒只收到 46,080 個取樣 —— 而我們用
`SetFrequency` 告訴它「請用 48000Hz 播」。它的 DAC 追一個永遠追不上的來源，
只能週期性補洞，那就是「啵啵啵」，而且**音量越大越明顯**（波形不連續，
不是底噪）。

修法是 `PLLQ = 24`（1152 / 24 = 48.000MHz，剛好整除）。
PLL3Q 在這個範例裡**只有 USB 一個使用者**，改它不會踩到別人
（對照 board-notes 23.1 —— 相簿那邊 PLL3 是三個人在搶）。
植入 `tools/patch_usbaudio.py`。

### 量出來的前後對照

同一份 Appli，只改 bootloader 的一個數字：

| 每秒 | 修之前 | 修之後 |
|---|---|---|
| SOF | **960** | **1000** |
| 送出封包 | 960 | **1000** |
| 45 秒缺的訊框 | **1806** | **8**（全在開播頭 3 秒）|
| 讀卡 | 360 x 512 = 184,320 B/s | **375 x 512 = 192,000 B/s** |

192,000 B/s 正好是 48000 x 2ch x 2byte 的即時速率。
原始記錄：`frames-before-pll3q-fix.log` 與 `frames-after-pll3q-fix.log`。

### 找到它的過程（三把尺，前兩把都量錯東西）

值得記下來，因為**每一把尺都「量到了數字」，但只有第三把量到病灶**：

1. **緩衝大小掃描**（上一輪）→ 非單調。量的是開播競爭跑了幾圈，
   跟緩衝無關。
2. **runway / over**（掏空與衝出界）→ 兩個都是陰性。
   `runway_min` 死死釘在 8320 bytes（43ms）、`over=0`。
   緩衝機制從頭到尾都是健康的 —— 上一輪掃參數本來就掃不到東西。
   **而且這把尺天生看不到病灶**：訊框沒送出去不會消耗緩衝，
   所以 runway 永遠好看。
3. **封包數 vs 毫秒數** → `pk=960 / ms=1000`，死板的 0.96。
   整數對整數，沒有解析度問題。

第 3 把尺量到之後還錯了一次假設：以為「一次呼叫最多送一包，所以上限是主
迴圈速率」。加一個計數器就推翻了 —— `it=162000`，主迴圈每毫秒跑 162 圈，
根本不是瓶頸。**再加 `sof` 這一欄，`pk == sof` 完全吻合，才把「我們漏送」
與「匯流排本身就慢」分開。**

順帶一個關於觀察手段的教訓：第 2 把尺每秒印一行 85 個字元，
@115200 就要 7.4ms，量到的 `gap_max` 剛好是 7ms —— **尺自己就是最大的
干擾源**。改成「收集期間完全不印、整表最後倒出來」之後 `gap` 降到 1ms。

## 已修掉的：週期性爆音（原始記錄留作對照）

修掉上面兩個之後「好很多但還有一點」，剩下的是「啵啵啵」的週期性爆音，
**而且音量越大越嚴重**（＝波形不連續，不是底噪）。

試過調緩衝大小，結果**非單調**：

| 緩衝 | 每 45 秒重新開播 |
|---|---|
| 512 x 33 = 16.9KB（原廠）| 4 次 |
| 2048 x 15 = 30.7KB | **78 次** |
| 4096 x 16 = 65.5KB | 36 次 |

非單調通常代表「我對它的模型是錯的」，該換方法而不是繼續試參數，
所以停在這裡。**後來查出模型錯在哪了，見下面〈爆音的模型錯在哪〉——
這張表量的不是緩衝掏空。**

爬文的結論跟量到的一致：

- 緩衝掏空是這個場景造成爆音的典型原因
- **主機端必須處理裝置的 feedback endpoint，否則漂移會累積** ——
  而 `usbh_audio.c` 整份**沒有出現過 feedback / synch 這些字**
- ST 社群 2026-02 有一篇〈Heavy Noise and Distorted Audio Output with
  USB Audio Class〉，症狀相符

## 量到了：端點是 **ADAPTIVE**，不必實作 feedback 端點

2026-08-26 實測。`audio.c` 的 `USB_DumpCfgDesc()` 在類別剛掛上、還沒碰 SD 卡
的時候，把整份設定描述元原封不動印到 COM4：

```
=== CFGDESC BEGIN VID=0ECB PID=208A dev_total=228 stored=228 ===
...
080: 01 80 BB 00 09 05 01 09 C0 00 01 00 00 07 25 01
...
ISO EP 01 OUT if=1 alt=1 attr=09 sync=ADAPTIVE usage=data mps=192 x1 bInterval=1 bLength=9 bSynchAddress=00
CLASS PICK: headphone supported=1 if=1 alt=1 Ep=01 EpSize=192 Poll=1
```

`09 05 01 09 C0 00 01 00 00` = 端點描述元：`bmAttributes = 0x09`
= `0000 1001`，bits[1:0]=`01` 等時、**bits[3:2]=`10` = adaptive**、
bits[5:4]=`00` 資料端點。而且 if=1 alt=1 的 `bNumEndpoints` 就是 1、
`bSynchAddress = 0x00` —— **整個裝置沒有任何 feedback 端點**，兩邊互相佐證。

**結論：時基是這塊板子的，dongle 跟著我們走。** 不必實作 ST 沒有的
feedback 端點，剩下的是緩衝設計 —— 那正是這個專案已經證明做得好的事。

順帶從同一份描述元讀到的（都是接下來用得到的硬條件）：

| | |
|---|---|
| 喇叭串流支援的取樣率 | **只有 48000 Hz 一個**（`bSamFreqType=1`）。影片音軌本來就是 48k，剛好 |
| 格式 | PCM、2 聲道、16 bit（`bSubframeSize=2` / `bBitResolution=16`）|
| 封包 | 192 bytes、`bInterval=1` —— 每 1ms 剛好 48 個立體聲取樣框 |
| 取樣率控制 | `07 25 01 01 00 00 00`，CS_ENDPOINT 的 `bmAttributes` bit0 = 1，所以 SET_CUR SAMPLING_FREQ 真的支援（跟 `SetFrequency OK` 對得起來）|
| 麥克風 | if=2 alt=1，EP `0x81` 也是 adaptive，支援 16k / 48k |
| `bMaxPower` | `0x0A` = 20mA |

換一支耳機要重測時，這段程式碼留著，插上去讀 COM4 就有。

## 爆音的模型錯在哪：那個計數量的不是緩衝掏空

前面「調緩衝大小，重新開播次數 4 / 78 / 36 非單調」的量測，
**量的根本不是緩衝掏空**。查證過的因果鏈：

1. `USBH_AUDIO_GetOutOffset()` 在 `play_state != AUDIO_PLAYBACK_PLAY` 時
   回 `-1` —— **「還沒開始播」跟「檔案播完了」回同一個值**。
   `AUDIO_Process` 把 `-1` 一律當成檔案結束 → `[NEXT]`。
2. 真正會讓 `play_state` 變成 PLAY 的只有 `USBH_AUDIO_Play()`，
   而它唯一的呼叫點是 `USBH_AUDIO_FrequencySet()` 這個回呼
   （`usbh_audio.c:1697`，SetEndpointControls 完成的那一刻）。
3. 我們的 `USBH_AUDIO_FrequencySet()` 只在
   `AUDIO_app_state == AUDIO_STATE_PLAYBACK_CONFIG` 時才呼叫 `Play` ——
   但狀態機下一輪就離開 CONFIG 了。**回呼落在 CONFIG 之外就整輪白跑**，
   於是 `[NEXT]` → CONFIG → START → PLAY → `-1` → `[NEXT]`，繞圈子，
   直到某一輪剛好撞上為止。

所以那三個數字是**這場競爭跑了幾圈**，不是緩衝掏空幾次。
競爭的圈數當然跟緩衝大小沒有單調關係 —— 非單調不是雜訊，是在說
「你量的東西跟你以為的不是同一件事」。

本輪實測的開機記錄看得到這個形狀：先連續 20 幾組
`[NEXT]` / `*** Playing audio ***` 快速刷過，之後才穩定播下去、
24 秒內一次都沒再重開。

**這是 ST 範例本來就有的競爭**（原版也只在 CONFIG 呼叫 `Play`），
2026-08-26 第一版 patch 沒有修掉它。

### 修法（已驗證）

兩件事一起做，少一件都看不出有沒有修好：

1. **開播不再等回呼。** `USBH_AUDIO_Play()` 只要求
   `play_state == AUDIO_PLAYBACK_IDLE`，而 SET_EP 完成之後 `play_state`
   就停在 IDLE 不會再變 —— 所以根本不必剛好在回呼那一瞬間呼叫。
   改成在 PLAY 狀態裡主動重試到 `USBH_OK`，跟 SetFrequency 的重試同一個
   寫法。`USBH_AUDIO_FrequencySet()` 降級成只印一行，不再負責開播。
2. **把「還沒開播」「播完了」「非預期停下來」分成三條路。**
   真正的檔案結束改用 `USBH_AUDIO_BufferEmptyCallback()` —— 那是
   `USBH_AUDIO_OutputStream` 在 `global_ptr > total_length` 時才呼叫的，
   是唯一可信的結束訊號。剩下的 `GetOutOffset() < 0` 現在會出聲
   （`非預期停止 #n`）而不是被靜悄悄當成檔案結束。

順帶更正上一輪對 `USBH_BUSY` 的解釋：`USBH_AUDIO_SetFrequency` 回 BUSY
**不是因為控制傳輸還沒完成**，而是因為它要求 `play_state` 是
`AUDIO_PLAYBACK_IDLE`，而類別剛掛上時是 `AUDIO_PLAYBACK_INIT`。
重試是對的，理由不一樣。

### 修完的實測

| | 修之前 | 修之後 |
|---|---|---|
| 開播前的 `[NEXT]` 空轉 | 20 幾圈 | **0** |
| 64 秒內重新開播 | 4 次 / 45 秒 | **0** |
| `*** Playing audio ***` | 每重開一次印一次 | **1 次** |
| `非預期停止` | （以前偵測不到）| **0** |

緩衝大小維持原廠的 512 x 33，**一個參數都沒有動**。

**還沒驗證的**：耳朵聽起來的「啵啵啵」有沒有跟著消失。
重新開播歸零證明的是那個機制被修好了，不等於爆音沒有第二個來源。

## 換一支耳機還能用嗎

不保證。**2026-08-26 實測兩支，一支通一支不通** —— 詳見下面 ROG 那一節。
六個關卡：UAC1 vs **UAC2**（ST 只實作 UAC1）、描述元排法、
多個 alt setting 要按格式挑（已修成通用機制）、取樣率清單、複合裝置、
**廠商私有初始化（這一關過不了就是過不了）**。

**挑選標準**：盒子上寫「PS4／PS5／Switch 免軟體直接用」——
那兩台主機只認 UAC1，JBL 那盒就有寫，結果也真的通。
反過來，主打「24-bit/96kHz 無線」的通常是 UAC2，風險高。

測一支新的只要兩分鐘：燒這個治具、插上去、讀 COM4。


## 音量：裝置的實際刻度，以及一個 ST 沒做完的 HID

JBL Quantum TWS 回報的音量刻度（`GET_MIN` / `GET_MAX` / `GET_RES`）：

| | |
|---|---|
| `volumeMin` | `0xB600` = **-74 dB** |
| `volumeMax` | `0xFFFF` = **0 dB** |
| `resolution` | `256` = **每格 1 dB** |
| 開機預設 | **不固定**（實測 -58 dB、-48 dB 都出現過）|

UAC1 的音量是**有號 16 bit、單位 1/256 dB**，而 ST 把它存進 `uint32_t`
（`AUDIO_ControlAttributeTypeDef.volume`），所以要自己轉回 `int16_t` 才有
意義。

**因為開機預設不固定，「按 N 下 VOLUME_UP」不是可重現的設定。**
治具改成算絕對目標：`AUDIO_VOL_PERCENT` 在 min..max 之間取百分比，
再一步一步走過去。目前是 **57% = -32 dB** —— 這個數字是使用者戴著 JBL
聽出來的，不是算出來的。dB 是對數刻度，百分比只是給人調的把手。

一次音量變更實測要**約 550ms 才落地**，所以每 120ms 發一次會有四五次
被蓋掉（類別的 `control_state` 只有一格，後蓋前）。無害，因為迴圈每次都
重讀目前值、到位就停。**我試過改成「上一步落地才發下一步」，結果衝過頭，
而且順手把診斷行變成每 120ms 印一次 —— 45 秒缺的訊框從 8 個變成 683 個。
會動的東西不要為了漂亮去改。**

### 耳機上的觸控滑動調不了音量（查清楚了，不是耳機的問題）

描述元裡有 HID 介面：`09 04 03 00 01 03 00 00 06`（介面 3、class 3）、
`09 21 10 01 00 01 22 A4 02`（**報告描述元 0x02A4 = 676 bytes**）、
`07 05 83 03 40 00 03`（EP `0x83` 中斷輸入、64 bytes、bInterval 3）。
手勢確實有回報上來，ST 的類別也真的開了管線在輪詢。

問題在 `USBH_AUDIO_Control()`（`usbh_audio.c:1605`）：

```c
attribute = LE16(&AUDIO_Handle->mem[0]);
USBH_AUDIO_SetControlAttribute(phost, (uint8_t)attribute);
```

它**直接把報告的第一個位元組當成「1 = 音量加、2 = 音量減」**，而且從頭到尾
沒有去抓那 676 個位元組的報告描述元。真正的 HID 報告第一個位元組通常是
Report ID。JBL 送上來的對不上，`SetControlAttribute` 回 `USBH_FAIL`，
所以什麼都沒發生。

**反過來是個隱患**：Report ID 剛好是 1 或 2 的裝置非常常見 ——
那種裝置在耳機上亂摸就會莫名其妙改到音量。接進相簿前要知道這件事。

決定**不做**：使用者說音量之後由相簿的螢幕控制項負責，
耳機端的手勢沒有非解不可。要做的話第一步是把 EP `0x83` 收到的原始報告
印出來看手勢送了什麼 —— 那是量測，不是猜。

## PLL3Q 撞車：已解決（HSI48 + CRS 校準到 LSE）

USB 全速要 48.000MHz，而相簿的 PLL3 三個輸出都名花有主（board-notes 23.1：
PLL3R 給 LTDC 的像素時脈、PLL3Q 給 I2S 的 49.1521MHz），而且
**LTDC 只有 PLL3R 一個來源**（`RCC_LTDCCLKSOURCE_PLL3R`，標著 unique），
所以 PLL3 放不掉。

**改分頻也救不了**：相簿的 PLL3 VCO 是 393.2168MHz，393.2168 / 48 = 8.192，
沒有任何整數分頻能生出 48MHz。要生出來就得動 VCO，而 VCO 一動 LTDC 的像素
時脈就跟著動。

### 三條路，兩條走不通

`RCC_USBOTGFSCLKSOURCE_*` 有四個選項（HSI48 / PLL3Q / HSE / CLK48）：

| 來源 | 結果 |
|---|---|
| **HSE**（24MHz）| 不是 48MHz，用不了 |
| **CLK48**（USBPHYC 的輸出）| **沒有時脈**。韌體卡在 `HAL_HCD_Init` 之前，UART 連開機橫幅都沒印。USBPHYC 的 PLL 不是選一選多工器就會跑，它要由 USB HS 那顆核心啟動，而我們只用 FS |
| **HSI48 原生** | **+3177 ppm**。USB 全速的訊框時序規格是 ±0.05%，超出六倍 —— **dongle 一直斷線重連**，聲音只出來一瞬間。這不是音質問題，是根本連不住 |
| **HSI48 + CRS(LSE)** | **可行。-44 ppm、開機後 0 次斷線** |

### 做法

CRS 是晶片內建的時脈校準單元，可以拿 LSE（32.768kHz 晶振）當基準持續修
HSI48。**分頻剛好整除**，所以量化誤差是 0：

```
SYNCDIV = 32 -> 48e6 x 32 / 32768 = 46875  （RELOAD = 46874，16 bit 放得下）
```

**實測 STM32H7S78-DK 有 32.768kHz 晶振**（`LSE ready`）—— 這個 repo 以前
從來沒碰過 LSE，是這一輪才驗出來的。程式碼在
`patches/usbh_conf-clock.c.frag`，由 `tools/patch_usbaudio.py` 自動套用，
起不來時會退回 PLL3Q 並在 COM4 印出走了哪一條路。

### 實測（`clock-hsi48-crs-lse.log`）

| | PLL3Q | HSI48 原生 | **HSI48 + CRS(LSE)** |
|---|---|---|---|
| 時脈誤差 | 0 ppm | +3177 ppm | **-44 ~ +200 ppm** |
| 開機後斷線 | 0 | **一直重連** | **0** |
| 45 秒缺的訊框 | 2~4 | n/a | **0~7** |
| 跟相簿撞車 | **會** | 不會 | **不會** |

**相簿的 PLL3 完全不用動** —— LTDC 和 3.5mm 的 I2S 都保持原樣，
兩條音訊輸出可以並存。

## 治具還沒修好的（都是鷹架，不影響上面的結論）

- **音量爬升會走錯方向。** 實測 `VOL 時間到：-35dB -> -29dB（目標 -37dB）`
  —— 該往下走 2dB，結果往上跑了 6dB。根因沒查：`USBH_AUDIO_SetControlAttribute`
  每設一次就把 `cs_req_state` 設成 `AUDIO_REQ_GET_VOLUME` 去讀回裝置回報的
  值，那個值不會停在我們算的目標上，所以「到位」的判斷從來不成立。
  一開始用步數上限擋，結果迴圈每 120ms 發一筆控制傳輸**整場不停**；
  改成 12 秒的時限之後至少會停。
  **相簿那邊音量會是螢幕上的控制項，不走這條路**，所以沒有繼續修。
- 開播頭十幾秒音量會由小變大，那是爬升本身，不是故障。


## 換一支耳機：ROG Delta II (2.4GHz) 實測（2026-08-26）

`VID 0x0B05 / PID 0x1AFA`、ASUSTek、UAC1、`bMaxPower` 100mA。
**結論：目前不能用**，但過不了的關卡不在 UAC1 這一層。

### 通過的關卡（全部量測驗證過）

| | |
|---|---|
| 列舉 / UAC1 類別掛得上 | ✅ |
| 端點同步型別 | ✅ 兩個 alt 都是 **adaptive**、`bSynchAddress=00`、無 feedback 端點 |
| 支援 48000Hz / 2ch / 16-bit | ✅（alt 1）|
| 裝置實際在 alt 1 | ✅ `GET_INTERFACE` 讀回 1（開播前後都是）|
| 端點取樣率 | ✅ `GET_CUR` 讀回 48000 |
| 靜音 | ✅ 讀回 0（本來就沒靜音）|
| 音量 | ✅ 寫 -25dB、讀回 -25dB |
| 麥克風介面放回零頻寬 | ✅ |
| 主機端串流 | ✅ 0 斷線、45 秒缺 4~7 個訊框、時脈 ±44ppm |

**該對的全部對了，就是沒有聲音。**

### 判讀

使用者的推論跟本檔〈換一支耳機還能用嗎〉那條判準一致：
**盒子上沒有寫「PS4／PS5／Switch 免軟體直接用」的，就是需要驅動。**
PS5 只認 UAC1、不裝驅動；ROG 的 2.4GHz dongle 若只主打 PC
（要 Armoury Crate），它需要的廠商私有初始化就不在 UAC1 裡。

要往下追只有一條路：**抓一次電腦上正常運作的 USB 匯流排封包**，
看驅動送了什麼。這個治具做不到。

### 這一輪從 ROG 學到而且留下來的東西

1. **按格式挑 alt setting**（`patches/usbh_audio-pick-alt.c.frag`）。
   ST 的 `USBH_AUDIO_InterfaceInit` 是「挑端點最大的」，ROG 因此被挑到
   24-bit 的 alt 2。**必須改在 InterfaceInit 裡** —— 它選完 alt 緊接著
   就用 `headphone.EpSize` 開管線，改晚了主機通道會照 576 排程而端點是
   384。這是通用機制（PS5／Windows／ALSA 都這樣做），不是 ROG 專用補丁；
   JBL 只有一個 alt，回測確認完全不受影響。
2. **ST 的類別從來不解除靜音。** `grep MUTE usbh_audio.c` 是空的。
   裝置若是靜音狀態，主機端會看到一個完美的串流而使用者什麼都聽不到。
3. **音量要讀寫「有那個控制的聲道」。** ROG 的 `bmaControls[0]=0x01`
   —— 主聲道**只有靜音沒有音量**。我一開始讀聲道 0，得到 -74dB，
   誤判成「音量卡在最小值」白追了一輪。
4. **不要在類別跑起來之後從外面搶 alt setting。** 那是跟類別搶
   `phost->Control` 的所有權，實測會互相覆蓋。要改就趁
   `usbh_core.c` 在 `Init()` 之後、`ClassRequest` 之前的那個視窗。

### 我自己的量測工具犯的錯（值得記）

readback 的緩衝我先清成 `0`，於是**「讀到 alt 0」和「這筆傳輸沒發生」
變成同一個值** —— alt 0 是零頻寬設定，是合法答案。靠這個誤判了兩輪，
直到某次 `MUTE` 讀回 255（另一個哨兵）才露餡。
改成 `0xAA` 之後所有讀數立刻一致。

**這正是 board-notes 8.7 那條「診斷變數要能分辨沒跑和跑了但結果是 0」，
而我是在自己的量測工具上犯的。**
