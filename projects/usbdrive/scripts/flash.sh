#!/usr/bin/env bash
#
# 把隨身碟 app 燒到外部 Flash 的 0x71000000。
#
# **不會動到 0x70000000 的相簿**，兩個 app 並存。相簿開機讀 VBUS，
# 插著 USB 線就跳過來（見 projects/album/app_src/usbdrive.c）。
#
# bootloader 不歸這裡管 —— 用 projects/album/scripts/flash.sh --boot，
# 而且**那顆 bootloader 必須是有補開 HSE 的版本**（album 的 patch_project.py
# 會植入），否則 USB 的 PHY 沒有時脈，完全不會列舉。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REPO=$(cd "$ROOT/../.." && pwd)

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
EL="$(dirname "$CLI")/ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr"

CUBE="${CUBE_DIR:-$REPO/cube}"
ELF="$CUBE/Projects/STM32H7S78-DK/Applications/USB_Device/MSC_Standalone/STM32CubeIDE/Appli/Debug/MSC_Standalone_Appli.elf"

[ -f "$ELF" ] || { echo "找不到 $ELF，請先跑 scripts/build.sh"; exit 1; }

# 連結位址要對，不然會蓋掉相簿。這個檢查很便宜，但錯了的代價是相簿不見。
if ! "$TOOLS.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin/arm-none-eabi-objdump.exe" -h "$ELF" | grep -q "71000000"; then
    echo "!! 這個 ELF 不是連結在 0x71000000，燒下去會蓋掉相簿"
    echo "   請先跑 scripts/build.sh（會套用 tools/patch_project.py）"
    exit 1
fi

echo "==> 燒錄隨身碟 app 到 0x71000000"
"$CLI" -c port=SWD mode=UR -el "$EL" -w "$ELF" | grep -iE "error|complete" || true

echo "==> 重置"
"$CLI" -c port=SWD mode=HOTPLUG -rst | grep -i "reset is performed" || true

echo
echo "用法：USB1（CN18）插上電腦 -> 自動變隨身碟；拔線後按一下板子的 reset 回相簿。"
