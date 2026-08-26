# USB 無線耳機（USB Host Audio）— 可行性已驗證，尚未整合

目標：**讓相簿的影片播放走無線耳機**。這個目錄是通往那個目標的第一步，
狀態是「**可行性已證明、音質還不行、還沒接進相簿**」。

## 一句話結論

**這塊板子可以當 USB 主機驅動 2.4GHz 無線耳機的 dongle。**
實測 JBL Quantum TWS（VID `0x0ECB` / PID `0x208A`）列舉成功、
ST 的 UAC1 類別掛得上、從 SD 卡串流 wav、耳機真的出聲。

但 **ST 的範例不足以做產品**，音質有週期性爆音（見下）。

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

## ST 範例的兩個真 bug（改掉才聽得到正常聲音）

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

## 還沒解決：週期性爆音

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
`patches/audio.c.patched` 那版沒有修掉它。修法是把「開播」從狀態的巧合
改成明確的條件，並且**先把「還沒開播」跟「播完了」分開**
—— 兩者現在共用 `-1`，不分開的話修完也看不出有沒有修好。

## 換一支耳機還能用嗎

不保證。五個關卡：UAC1 vs **UAC2**（ST 只實作 UAC1）、描述元排法、
取樣率清單（類別只處理最多 5 個離散值）、複合裝置、供電。

**挑選標準**：盒子上寫「PS4／PS5／Switch 免軟體直接用」——
那兩台主機只認 UAC1，JBL 那盒就有寫，結果也真的通。
反過來，主打「24-bit/96kHz 無線」的通常是 UAC2，風險高。

測一支新的只要兩分鐘：燒這個治具、插上去、讀 COM4。
