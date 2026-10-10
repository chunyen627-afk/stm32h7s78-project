#!/usr/bin/env bash
# 開發用燒錄：原廠 bootloader 進內部 Flash、儀表進外部 Flash，讀回比對。
# 要燒回別的專案用 repo 根目錄的 firmware/restore.sh。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REPO=$(cd "$ROOT/../.." && pwd)
export PYTHONIOENCODING=utf-8

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
GCC="$TOOLS.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin"
EL="$(dirname "$CLI")/ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr"
PROJ="$REPO/cube/Projects/STM32H7S78-DK/Templates/RaceDash"
ELF="$PROJ/STM32CubeIDE/Appli/Debug/RaceDash_Appli.elf"

[ -f "$ELF" ] || { echo "找不到 $ELF，請先跑 scripts/build.sh"; exit 1; }

echo "==> bootloader（原廠）-> 內部 Flash"
"$CLI" -c port=SWD mode=UR -w "$PROJ/Binary/Boot_XIP.hex" -v | grep -iE "error|verified" || true

echo "==> 儀表 -> 外部 Flash"
"$CLI" -c port=SWD mode=UR -el "$EL" -w "$ELF" | grep -iE "error|complete" || true

echo "==> 讀回比對"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$GCC/arm-none-eabi-objcopy.exe" -O binary "$ELF" "$TMP/want.bin"
SIZE=$(stat -c %s "$TMP/want.bin")
"$CLI" -c port=SWD mode=UR -el "$EL" -r 0x70000000 "$SIZE" "$TMP/got.bin" >/dev/null 2>&1
python -c "
import sys
w=open(sys.argv[1],'rb').read(); g=open(sys.argv[2],'rb').read()
bad=[i for i in range(min(len(w),len(g))) if w[i]!=g[i]]
print('  讀回比對：完全相符' if not bad and len(g)>=len(w) else f'  !! 不符 {len(bad)} 個位元組，第一個在 0x{0x70000000+bad[0]:08X}' if bad else '  !! 讀回長度不足')
" "$TMP/want.bin" "$TMP/got.bin"

echo "==> 重置"
"$CLI" -c port=SWD mode=HOTPLUG -rst | grep -i "reset is performed" || true
