# 2.8 吋 ESP32（ESP32-2432S028）韌體：原作 + RaceDash 展示模式

原作：caa1211/esp32-gt7-dashboard v2.0.1（MIT），環境 `esp32-st7789`（這塊是 USB-C + micro-USB
雙孔版，螢幕 ST7789；燒 ILI9341 版會左右鏡像、偏藍）。

改了什麼（只動 `src/SHCustomProtocol.h`，另加 `src/RaceDashDemo.h`）：
- 畫面上的 SimHub 字樣換成 RaceDash（跟 H7 的 `gt7dash/orig/SHCustomProtocol.h` 同一份）
- 設定主選單多一顆 DEMO：板子自己模擬跑圈（跟 H7、Bridge 的展示模式同一套，`bridge/demo.c`）
- DEMO 中點螢幕：DEMO 選單（< PREV / NEXT > 換主題、STOP DEMO、BACK）
- 換主題時釋放舊主題的離屏畫布（`patch_sprites.py`）：原作建了就不放，ESP32 換過幾個主題後
  記憶體不夠 → Pocket 檔位框空白、Classic 圈速格與油門條、Endurance 本圈時間改直接畫而閃爍

重做：
```bash
git clone https://github.com/caa1211/esp32-gt7-dashboard.git mod_src      # main = v2.0.1（沒有 tag）
cp ../gt7dash/orig/SHCustomProtocol.h mod_src/src/                        # 換字樣的版本
python gen_demo.py ../bridge/demo.c mod_src/src/RaceDashDemo.h
python patch_demo.py mod_src/src/SHCustomProtocol.h
python patch_sprites.py mod_src/src
cd mod_src && pio run -e esp32-st7789
esptool --port COM7 write-flash 0x10000 .pio/build/esp32-st7789/firmware.bin   # 燒前先關 RaceDash Bridge
```
`firmware-st7789-racedash.bin` 是編好的（燒在 0x10000，設定與觸控校正會保留）。
