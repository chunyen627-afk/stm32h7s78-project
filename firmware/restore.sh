#!/usr/bin/env bash
#
# 把某個專案整套燒回板子。只需要 STM32CubeProgrammer，不需要 CubeIDE 或
# 540MB 的 ST 韌體包 —— 燒的是這個目錄裡存好的快照，跟當下的建置環境無關
# （別的專案把 cube/ 改成什麼樣都不影響）。
#
#   ./firmware/restore.sh 相簿          相簿（含影片播放、USB 隨身碟、USB 耳機）
#   ./firmware/restore.sh 俄羅斯方塊
#   ./firmware/restore.sh 影片燒卡工具  獨立影片播放器＋用 ST-LINK 寫 SD 卡
#
# **每次都整套燒，連 bootloader 一起。** 別的專案常會換掉內部 Flash 的
# bootloader，而相簿那顆有補開 HSE —— 少了它隨身碟模式會完全不列舉，
# 而且沒有任何錯誤訊息（踩過）。整套燒多花不到十秒，換來永遠不必想。
#
# 外部 Flash 用讀回比對驗證，不用 CLI 的 -v：那個只會丟一句「Data mismatch」
# 就中止，看不出壞在哪（board-notes 10.5 / 17.8）。
#
set -e
export PYTHONIOENCODING=utf-8   # 讀回比對的中文訊息在 Windows 主控台才不會變亂碼
cd "$(dirname "$0")"
HERE=$(pwd)

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
EL="$(dirname "$CLI")/ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr"

[ -f "$CLI" ] || { echo "找不到 STM32_Programmer_CLI：$CLI"; exit 1; }

# 名稱 -> 目錄。中英文、常見說法都收；使用者心裡「影片播放」「隨身碟」
# 「耳機」都是相簿的功能，不是獨立的東西。
case "$1" in
    相簿|album|電子相簿|影片|影片播放|隨身碟|USB隨身碟|耳機|USB耳機|無線耳機)
        NAME=album ;;
    俄羅斯方塊|方塊|tetris)
        NAME=tetris ;;
    影片燒卡工具|影片播放器|燒卡|video|video_player)
        NAME=video ;;
    *)
        echo "用法：$0 <專案>"
        echo "  相簿          電子相簿（含影片播放、USB 隨身碟、USB 耳機）"
        echo "  俄羅斯方塊"
        echo "  影片燒卡工具  獨立影片播放器＋ST-LINK 寫 SD 卡"
        exit 1 ;;
esac

# 每個專案燒哪些東西：檔名:外部 Flash 位址。bootloader 另外處理。
case "$NAME" in
    album)  ITEMS="album.bin:0x70000000 usbdrive.bin:0x71000000" ;;
    tetris) ITEMS="tetris.bin:0x70000000" ;;
    video)  ITEMS="video_player.bin:0x70000000" ;;
esac

DIR="$HERE/$NAME"

# 先確認板子接著。沒接的話後面每一步都會失敗，訊息又分散，一眼看不出原因。
if "$CLI" -l st-link 2>&1 | grep -q "No ST-Link detected"; then
    echo "!! 偵測不到 ST-LINK：板子沒接上電腦（CN7 那個 USB 孔）。什麼都沒燒。"
    exit 1
fi

echo "==> 燒回「$1」（firmware/$NAME）"

echo "==> bootloader -> 內部 Flash"
"$CLI" -c port=SWD mode=UR -w "$DIR/boot.hex" -v |
    grep -iE "error|complete|verified" || true

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAIL=0

for it in $ITEMS; do
    F="${it%%:*}"
    A="${it##*:}"
    SIZE=$(stat -c %s "$DIR/$F")

    echo "==> $F（$((SIZE / 1024)) KB）-> 外部 Flash $A"
    "$CLI" -c port=SWD mode=UR -el "$EL" -w "$DIR/$F" "$A" |
        grep -iE "error|complete" || true

    # 一行寫完，不用行尾的反斜線：Windows 行尾下續行會失效（build.sh 踩過）。
    "$CLI" -c port=SWD mode=UR -el "$EL" -r "$A" "$SIZE" "$TMP/got.bin" >/dev/null 2>&1
    python - "$DIR/$F" "$TMP/got.bin" "$A" <<'PY' || FAIL=1
import sys
want = open(sys.argv[1], 'rb').read()
got  = open(sys.argv[2], 'rb').read()
base = int(sys.argv[3], 16)
bad = [i for i in range(min(len(want), len(got))) if want[i] != got[i]]
if len(got) < len(want):
    print(f"  !! 讀回只有 {len(got)} bytes，應為 {len(want)}")
    sys.exit(1)
if not bad:
    print(f"  讀回比對：完全相符")
    sys.exit(0)
print(f"  !! 讀回有 {len(bad)} 個位元組不符：")
for i in bad[:8]:
    print(f"     0x{base + i:08X}  應為 0x{want[i]:02X} 實際 0x{got[i]:02X}")
print("  舊板子有已知固定壞格（0x70170466、0x700217C6，都是 bit 2）；")
print("  現在這塊板子實測零壞格，出現不符就要查。")
sys.exit(1)
PY
done

echo "==> 重置"
"$CLI" -c port=SWD mode=HOTPLUG -rst | grep -i "reset is performed" || true

if [ "$FAIL" -ne 0 ]; then
    echo "!! 有檔案讀回不符，見上面"
    exit 1
fi
echo "==> 完成"
