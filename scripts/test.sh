#!/usr/bin/env bash
# 在 QEMU 上跑遊戲邏輯與繪圖層的測試（不需要板子）。
set -e
cd "$(dirname "$0")/.."

GCC="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin/arm-none-eabi-gcc.exe"
export PATH="$PATH:/c/Program Files/qemu"

fail=0
for t in core gfx input; do
    "$GCC" -o "test/test_$t.elf" "test/test_$t.c" test/startup_qemu.c \
        core/tetris.c core/gfx.c core/ui.c core/input.c core/font_zh.c \
        -Icore -std=c11 -Wall -Wextra -O1 -g \
        -mcpu=cortex-m7 -mthumb -nostartfiles \
        -T test/mps2.ld -specs=rdimon.specs

    echo "--- $t ---"
    out=$(timeout 300 qemu-system-arm -M mps2-an500 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "test/test_$t.elf" 2>&1 | grep -v "RESERVED\|host_open")
    echo "$out" | tail -4
    echo "$out" | grep -q "ALL PASS" || fail=1
done
exit $fail
