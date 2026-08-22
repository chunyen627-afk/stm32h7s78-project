#!/usr/bin/env bash
#
# 讀出板子上的診斷計數器並算成人看得懂的數字。
#
# 這塊板子沒有 UART，狀態一律靠全域變數加 SWD。位址從 ELF 查，
# 改一行程式就會位移，所以不要寫死。
# 連線一定要用 mode=HOTPLUG —— UR 會把核心 halt 在 reset 點，
# 讀到的永遠是初始值（board-notes 9.1）。
#
set -e
cd "$(dirname "$0")/.."
REPO=$(cd "$(pwd)/../.." && pwd)

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
NM="$TOOLS.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin/arm-none-eabi-nm.exe"

CUBE="${CUBE_DIR:-$REPO/cube}"
ELF="$CUBE/Projects/STM32H7S78-DK/Templates/Video/STM32CubeIDE/Appli/Debug/Video_Appli.elf"
[ -f "$ELF" ] || { echo "找不到 $ELF"; exit 1; }

"$NM" "$ELF" | awk '{print $1, $3}' > /tmp/vsyms.txt

read_sym() {
    local a; a=$(grep -E " $1\$" /tmp/vsyms.txt | awk '{print "0x"$1}')
    [ -n "$a" ] || { echo ""; return; }
    "$CLI" -c port=SWD mode=HOTPLUG -r32 "$a" 4 2>/dev/null |
        grep -E "^0x[0-9A-Fa-f]{8} : " | awk '{print $3}' | head -1
}

d() { echo $((16#$1)); }

SRC=$(read_sym g_dbg_src)
STAGE=$(read_sym g_dbg_stage)
NF=$(read_sym g_dbg_nframe)
FAIL=$(read_sym g_dbg_fail)
FPS=$(read_sym g_dbg_fps_x100)
LATE=$(read_sym g_dbg_late)
CNT=$(read_sym g_dbg_hdr_count)
FU=$(read_sym g_dbg_ltdc_fu)
SDEC=$(read_sym g_dbg_sum_dec)
SCC=$(read_sym g_dbg_sum_cc)
SOUT=$(read_sym g_dbg_sum_out)
SALL=$(read_sym g_dbg_sum_all)
SRD=$(read_sym g_dbg_sum_read)
FSERR=$(read_sym g_dbg_fs_err)
SDST=$(read_sym g_sd_stage)

case "$(d "$SRC")" in
    0) S="沒有來源（SD 上沒有 video.bin，Flash 也沒有影格包）" ;;
    1) S="SD 卡 0:/video.bin" ;;
    2) S="外部 Flash 0x71000000" ;;
    *) S="?" ;;
esac

echo "來源      ：$S"
echo "階段      ：$(d "$STAGE")  （5=播放中 20=燒錄模式 91=找不到來源 92=DMA2D 失敗）"
echo "影格總數  ：$(d "$CNT")"
echo "已播/失敗 ：$(d "$NF") / $(d "$FAIL")"
echo "實測格率  ：$(awk -v v="$(d "$FPS")" 'BEGIN{printf "%.2f", v/100}') fps"
echo "跟不上次數：$(d "$LATE")"
echo "LTDC 斷流 ：$(d "$FU")"
[ -n "$FSERR" ] && echo "FatFs 錯誤：$(d "$FSERR")　SD 初始化階段：$(d "$SDST")"

N=$(d "$NF")
if [ "$N" -gt 0 ]; then
    echo
    echo "每格平均（微秒）："
    awk -v n="$N" -v rd="$(d "$SRD")" -v de="$(d "$SDEC")" -v cc="$(d "$SCC")" \
        -v ou="$(d "$SOUT")" -v al="$(d "$SALL")" 'BEGIN{
        printf "  讀檔    %8.2f ms\n", rd/n/1000
        printf "  解碼    %8.2f ms\n", de/n/1000
        printf "  轉色    %8.2f ms\n", cc/n/1000
        printf "  換頁    %8.2f ms\n", ou/n/1000
        printf "  合計    %8.2f ms  (含對時等待)\n", al/n/1000
        printf "  扣掉對時後的能力上限約 %.1f fps\n", 1000/((rd+de+cc+ou)/n/1000)
    }'
fi
