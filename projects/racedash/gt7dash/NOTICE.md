# 第三方程式碼與授權

## orig/ —— caa1211/esp32-gt7-dashboard（MIT License）

https://github.com/caa1211/esp32-gt7-dashboard ，v2.0.1。

`orig/` 底下的檔案從原作原封不動複製過來（SHCustomProtocol.h、SimHubProtocol.h、
TelemetrySource.h、DashboardWiFiCredentials.h、dashboard/ 底下的七個主題與圖示、
GT7DerivedMetrics、GT7UDPParser 與 Salsa20），`compat/version.h` 也是原作的
`include/version.h`。原作在 README 宣告採用 MIT License；著作權歸原作者所有。
`simhub/racedash-serial.js` 是原作 `simhub/custom-protocol.txt` 只改最後一行
（結尾加換行）。

GT7UDPParser 與 Salsa20 是原作從 MacManley/gt7-udp 收錄的（見原作的
platformio.ini 與 lib/GT7Udp/LICENSE）。

## lgfx/ —— LovyanGFX（FreeBSD License）

https://github.com/lovyan03/LovyanGFX ，原作專案收錄的版本。完整授權見
`lgfx/LICENSE-LovyanGFX.txt`（含 LovyanGFX 沿用的 Adafruit GFX、TFT_eSPI 等來源聲明）。
只取記憶體畫布、字型與工具函式；修補四處，都只在 `LGFX_RACEDASH` 時生效
（見 ../README.md）。

## hires/ —— 高解析度鏡像

- `stb_truetype.h`：https://github.com/nothings/stb v1.26，Sean Barrett，公有領域
  （或 MIT，擇一；授權寫在檔案結尾）。用它的掃描線把輪廓畫成抗鋸齒
- `hires_fonts.inc`：由 `tools/gen_hires_fonts.c` 從下面兩套字型取出 ASCII 字元的
  輪廓。原作的 FreeSans、DejaVu 點陣字（LovyanGFX 收錄）本來就是從這兩套轉出來的
  - **FreeSans**：GNU FreeFont 20120503，https://ftp.gnu.org/gnu/freefont/
    （`freefont-ttf-20120503.zip` 裡的 `FreeSans.ttf`）。GPLv3 或之後版本，
    附字型例外條款；輪廓資料沿用同一授權
  - **DejaVu Sans**：DejaVu Fonts 2.37，https://github.com/dejavu-fonts/dejavu-fonts
    （`dejavu-fonts-ttf-2.37.zip` 裡的 `DejaVuSans.ttf`）。Bitstream Vera 字型授權
    （DejaVu 的修改部分為公有領域）

TTF 本身不放在 repo 裡；要重新產生 `hires_fonts.inc` 時照上面的網址下載，然後：

```bash
gcc -O2 -o gen_hires_fonts hires/tools/gen_hires_fonts.c -lm
./gen_hires_fonts FreeSans.ttf DejaVuSans.ttf > hires/hires_fonts.inc
```
