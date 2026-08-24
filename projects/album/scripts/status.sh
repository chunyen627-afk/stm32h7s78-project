#!/usr/bin/env bash
#
# 讀相簿的全域狀態（板子沒有 UART，一律 SWD 讀）。
#
# 用 mode=HOTPLUG：不重置、不停核心，讀的是「正在跑的那一刻」。
# 用 mode=UR 會把板子重置，讀到的全是歸零後的初值 —— 那是白讀。
#
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REPO=$(cd "$ROOT/../.." && pwd)

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
NM="$TOOLS.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin/arm-none-eabi-nm.exe"

CUBE="${CUBE_DIR:-$REPO/cube}"
ELF="$CUBE/Projects/STM32H7S78-DK/Templates/Album/STM32CubeIDE/Appli/Debug/Album_Appli.elf"
[ -f "$ELF" ] || { echo "找不到 $ELF"; exit 1; }

SYMS=$(mktemp)
"$NM" "$ELF" > "$SYMS"

read_sym() {
    local a
    a=$(grep -E " $1\$" "$SYMS" | awk '{print "0x"$1}' | head -1)
    [ -n "$a" ] || { echo "-"; return; }
    "$CLI" -c port=SWD mode=HOTPLUG -r32 "$a" 4 2>/dev/null |
        grep -E "^0x[0-9A-Fa-f]{8} : " | awk '{print $3}' | head -1
}

# 一次連線讀多個會快很多，但先求正確：逐一讀。
for s in "$@"; do
    printf '%-20s %s\n' "$s" "$(read_sym "$s")"
done

rm -f "$SYMS"
