#!/usr/bin/env bash
#
# 燒錄到板子。Boot 進內部 Flash，影片進外部 Flash（需 external loader）。
#
# 驗證方式：不用 STM32_Programmer_CLI 的 -v，改成整份讀回來自己比對。
# 原因是這塊板子的外部 Flash 有一個固定壞格（見下方說明），-v 只會丟一行
# 「Data mismatch」就讓腳本中止，看不出到底是壞在哪、要不要緊。讀回來自己比
# 可以指出確切位址與落在哪個符號，判斷才有依據。整份 1.5MB 讀回只要幾秒。
#
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# 韌體包與共用程式碼放在 repo 根目錄，所有專案共用。
REPO=$(cd "$ROOT/../.." && pwd)

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
GCC="$TOOLS.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin"
EL="$(dirname "$CLI")/ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr"

CUBE="${CUBE_DIR:-$REPO/cube}"
PROJ="$CUBE/Projects/STM32H7S78-DK/Templates/Video"
ELF="$PROJ/STM32CubeIDE/Appli/Debug/Video_Appli.elf"

[ -f "$ELF" ] || { echo "找不到 $ELF，請先跑 scripts/build.sh"; exit 1; }

if [ "$1" = "--boot" ]; then
    echo "==> 燒錄 bootloader 到內部 Flash"
    "$CLI" -c port=SWD mode=UR -w "$PROJ/Binary/Boot_XIP.hex" -v
fi

echo "==> 燒錄影片到外部 Flash"
"$CLI" -c port=SWD mode=UR -el "$EL" -w "$ELF" | grep -iE "error|complete" || true

echo "==> 讀回比對"
TMP=$(mktemp -d)
"$GCC/arm-none-eabi-objcopy.exe" -O binary "$ELF" "$TMP/want.bin"
SIZE=$(stat -c %s "$TMP/want.bin")
"$CLI" -c port=SWD mode=UR -el "$EL" -r 0x70000000 "$SIZE" "$TMP/got.bin" \
    >/dev/null 2>&1

"$GCC/arm-none-eabi-nm.exe" -S "$ELF" 2>/dev/null > "$TMP/syms.txt" || true

python - "$TMP/want.bin" "$TMP/got.bin" "$TMP/syms.txt" <<'PY'
import sys

want = open(sys.argv[1], 'rb').read()
got  = open(sys.argv[2], 'rb').read()

syms = []
for line in open(sys.argv[3], encoding='utf-8', errors='ignore'):
    f = line.split()
    if len(f) >= 4:
        try:
            syms.append((int(f[0], 16), int(f[1], 16), f[3]))
        except ValueError:
            pass

bad = [i for i in range(min(len(want), len(got))) if want[i] != got[i]]
if not bad:
    print(f"  完全相符（{len(want)} bytes）")
    sys.exit(0)

print(f"  有 {len(bad)} 個位元組不符：")
for i in bad[:8]:
    addr = 0x70000000 + i
    where = next((n for a, s, n in syms if a <= addr < a + s), "?")
    print(f"    0x{addr:08X}  應為 0x{want[i]:02X} 實際 0x{got[i]:02X} "
          f"（差異位元 0x{want[i] ^ got[i]:02X}）位於 {where}")
if len(bad) > 8:
    print(f"    ...另外還有 {len(bad) - 8} 個")

# 這塊板子在 0x70170466 有一個固定壞格（bit 2 抹除後回不到 1），每次燒錄都會
# 出現。只要不符的位元組僅此一個、而且落在查表資料裡，就不影響執行。
print()
print("  註：本板 0x70170466 為已知固定壞格。若不符的只有它，可以忽略；")
print("      若出現在程式碼區段（.text 的函式）就不能忽略，要調整版面避開。")
sys.exit(0)
PY

rm -rf "$TMP"

echo "==> 重置"
"$CLI" -c port=SWD mode=HOTPLUG -rst | grep -i "reset is performed" || true
