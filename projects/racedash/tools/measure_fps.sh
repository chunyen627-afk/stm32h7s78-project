#!/usr/bin/env bash
# 驗證每個主題能不能 60Hz：板子自己產生資料（展示模式），間隔預設 16ms
# （= 授權版 SimHub 一秒 60 次），每個主題熱身 6 秒、量 10 秒。
# 經 SWD（HOTPLUG，不重置板子）讀寫 DTCM 的狀態格，見 README「除錯」。
#
#   tools/measure_fps.sh                 # 七個主題
#   THEMES="0 3" tools/measure_fps.sh    # 只量 Classic、Radar
#   PERIOD=100 tools/measure_fps.sh      # 免費版的一秒 10 次
#
# 量完會關掉展示模式、切回 GT3。SimHub 開著也可以量（展示模式時板子不理序列埠）。
TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
W() { "$CLI" -c port=SWD mode=HOTPLUG -w32 "$1" "$2" >/dev/null 2>&1; }
# 一次讀 20 格，同一瞬間的 tick、資料筆數、掉格、換頁次數
snap() { "$CLI" -c port=SWD mode=HOTPLUG -r32 0x20004020 80 2>&1 | grep -E "^0x2000" | awk '{for(i=3;i<=6;i++) printf "%d ", strtonum("0x"$i)}'; }

W 0x20004064 "${PERIOD:-16}"          # BOX[17] 資料間隔
W 0x2000404C 0x44454D4F               # BOX[11] "DEMO"
for t in ${THEMES:-0 1 2 3 4 5 6}; do
    W 0x20004050 $((0x54480000 + t))  # BOX[12] 切主題
    sleep 6                           # 熱身：第一次畫要產生字形
    W 0x20004060 0                    # BOX[16] 最慢一格重新量
    a=($(snap)); sleep 10; b=($(snap))
    # BOX：[1] tick  [3] 有效行  [16] 最慢一格  [18] 掉格  [19] 換頁
    awk -v t="$t" -v dt=$((b[1] - a[1])) -v dl=$((b[3] - a[3])) -v fr=$((b[19] - a[19])) \
        -v miss=$((b[18] - a[18])) -v mx="${b[16]}" 'BEGIN {
        split("Classic GT3 Retro Radar Mono Pocket Endurance", N, " ");
        printf "%-10s 每秒 %.1f 格  掉格 %d/%d  最慢 %.1fms  資料 %.0f 筆/秒\n",
               N[t + 1], fr * 1000 / dt, miss, fr, mx / 600000, dl * 1000 / dt }'
done
W 0x2000404C 0                        # 關掉展示模式
W 0x20004064 0
W 0x20004050 0x54480001               # 回到 GT3
