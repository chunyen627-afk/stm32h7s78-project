#!/usr/bin/env bash
#
# 把預編譯的韌體燒進板子。不需要 CubeIDE 專案，只要 STM32CubeProgrammer。
#
#   ./firmware/flash.sh --boot          新板子第一次：bootloader + 相簿
#   ./firmware/flash.sh                 只更新相簿
#   ./firmware/flash.sh video_player    換成影片播放器
#
# 驗證方式：不用 -v，改成整份讀回來自己比對。這塊板子的外部 Flash 有固定
# 壞格，-v 只會丟一行「Data mismatch」就中止，看不出壞在哪、要不要緊
# （board-notes 10.5 / 17.8）。
#
set -e
cd "$(dirname "$0")"
HERE=$(pwd)

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
EL="$(dirname "$CLI")/ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr"

[ -f "$CLI" ] || { echo "找不到 STM32_Programmer_CLI：$CLI"; exit 1; }

APP=album
if [ "$1" = "--boot" ]; then
    echo "==> 燒錄 bootloader 到內部 Flash（新板子只需一次）"
    "$CLI" -c port=SWD mode=UR -w "$HERE/Boot_XIP.hex" | grep -iE "error|complete" || true
elif [ -n "$1" ]; then
    APP="$1"
fi

BIN="$HERE/$APP.bin"
[ -f "$BIN" ] || { echo "找不到 $BIN"; exit 1; }
SIZE=$(stat -c %s "$BIN")

echo "==> 燒錄 $APP.bin（$((SIZE / 1024)) KB）到外部 Flash 0x70000000"
"$CLI" -c port=SWD mode=UR -el "$EL" -w "$BIN" 0x70000000 |
    grep -iE "error|complete" || true

echo "==> 讀回比對"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$CLI" -c port=SWD mode=UR -el "$EL" -r 0x70000000 "$SIZE" "$TMP/got.bin" \
    >/dev/null 2>&1

python - "$BIN" "$TMP/got.bin" <<'PY'
import sys
want = open(sys.argv[1], 'rb').read()
got  = open(sys.argv[2], 'rb').read()
bad = [i for i in range(min(len(want), len(got))) if want[i] != got[i]]
if not bad:
    print(f"  完全相符（{len(want)} bytes）")
else:
    print(f"  有 {len(bad)} 個位元組不符：")
    for i in bad[:8]:
        print(f"    0x{0x70000000 + i:08X}  應為 0x{want[i]:02X} 實際 0x{got[i]:02X}"
              f"（差異位元 0x{want[i] ^ got[i]:02X}）")
    print()
    print("  本板已知的固定壞格：0x70170466、0x700217C6（壞的都是 bit 2）。")
    print("  只有這些的話可以忽略；出現在別的位址就要查是不是新的壞格。")
PY

echo "==> 重置"
"$CLI" -c port=SWD mode=HOTPLUG -rst | grep -i "reset is performed" || true
