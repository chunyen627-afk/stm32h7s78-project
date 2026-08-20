#!/usr/bin/env bash
# 從編譯結果產生可提交的燒錄檔。
set -e
cd "$(dirname "$0")/.."

OBJCOPY="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin/arm-none-eabi-objcopy.exe"
T="cube/Projects/STM32H7S78-DK/Templates/Tetris"
ELF="$T/STM32CubeIDE/Appli/Debug/Template_XIP_Appli.elf"

[ -f "$ELF" ] || { echo "找不到 $ELF，請先跑 scripts/build.sh"; exit 1; }

mkdir -p firmware
# ELF 有 2MB 是因為含除錯符號；bin/hex 只有實際要燒的內容。
"$OBJCOPY" -O binary "$ELF" firmware/tetris_appli.bin
"$OBJCOPY" -O ihex   "$ELF" firmware/tetris_appli.hex
cp "$T/Binary/Boot_XIP.hex" firmware/boot_xip.hex

ls -la firmware/*.bin firmware/*.hex | awk '{printf "  %8d  %s\n", $5, $9}'
