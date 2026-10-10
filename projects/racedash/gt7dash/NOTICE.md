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
只取記憶體畫布、字型與工具函式；修補三處，都只在 `LGFX_RACEDASH` 時生效
（見 ../README.md）。
