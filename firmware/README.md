# 預編譯燒錄檔

不想自己建置的話，直接燒這裡的檔案就能玩。

## 檔案

| 檔案 | 用途 | 燒錄位置 |
|---|---|---|
| `boot_xip.hex` | Bootloader | 內部 Flash `0x08000000` |
| `tetris_appli.hex` | 遊戲本體 | 外部 Flash `0x70000000` |
| `tetris_appli.bin` | 同上，raw 格式 | 外部 Flash `0x70000000` |

`.hex` 內含位址資訊，直接燒即可。`.bin` 要自己指定位址。

## 硬體前提

- STM32H7S78-DK 開發板
- **BOOT0 滑動開關要在 SW1 位置**
- USB-C **資料線**（純充電線無法辨識）

## 燒錄步驟

需要 STM32CubeProgrammer（或 STM32CubeIDE 內附的 `STM32_Programmer_CLI`）。

```bash
CLI="STM32_Programmer_CLI"
EL=".../ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr"

# 1. Bootloader 進內部 Flash（只需第一次）
"$CLI" -c port=SWD mode=UR -w boot_xip.hex -v

# 2. 遊戲進外部 Flash，必須掛 external loader
"$CLI" -c port=SWD mode=UR -el "$EL" -w tetris_appli.hex -v

# 3. 重置
"$CLI" -c port=SWD mode=HOTPLUG -rst
```

**第 2 步漏掉 `-el` 會燒不進去**，因為程式本體在外部 Flash。
external loader 隨 STM32CubeProgrammer 一起安裝，路徑通常在
`.../STM32CubeProgrammer/bin/ExternalLoader/`。

用 GUI 版 STM32CubeProgrammer 的話，記得在 External loaders 分頁
勾選 `MX66UW1G45G_STM32H7S78-DK`。

## 更新這些檔案

```bash
./scripts/build.sh          # 重新編譯
./scripts/export.sh         # 產生 firmware/ 底下的檔案
```
