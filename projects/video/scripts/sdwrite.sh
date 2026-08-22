#!/usr/bin/env bash
#
# 透過 ST-LINK 把影片檔寫進板子上的 SD 卡。
#
# 電腦上沒有讀卡機，而影片檔有一百多 MB、放不進 128MB 的外部 Flash，
# 所以讓板子當中介：SWD 把資料塞進 PSRAM，韌體收一塊寫一塊到卡上。
# 實測 SWD 在 24MHz（ST-LINK V3 上限）約 895 KB/s。
#
# 前提：板子上跑的必須是含燒錄模式的影片韌體（scripts/build.sh + flash.sh）。
#
#   用法：sdwrite.sh <本機檔案> [--yes]
#
# 安全性：韌體只會開 0:/video.bin 這一個檔案，沒有格式化、沒有刪除、
# 沒有目錄走訪。卡上其他東西（例如相簿的照片）不會被動到。
#
set -e

# 先把來源轉成絕對路徑再切目錄，否則使用者給的相對路徑會相對錯地方。
[ -f "$1" ] || { echo "用法：sdwrite.sh <本機檔案> [--yes]"; exit 1; }
SRC=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")

cd "$(dirname "$0")/.."
ROOT=$(pwd)
REPO=$(cd "$ROOT/../.." && pwd)

TOOLS="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools"
CLI="$TOOLS.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
NM="$TOOLS.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin/arm-none-eabi-nm.exe"

CUBE="${CUBE_DIR:-$REPO/cube}"
ELF="$CUBE/Projects/STM32H7S78-DK/Templates/Video/STM32CubeIDE/Appli/Debug/Video_Appli.elf"
[ -f "$ELF" ] || { echo "找不到 $ELF，請先跑 scripts/build.sh"; exit 1; }

# 燒錄協定的旗標位址從 ELF 查，不要寫死 —— 改一行程式就會全部位移。
sym() { "$NM" "$ELF" | grep -E " $1\$" | awk '{print "0x"$1}'; }
GO=$(sym g_sdw_go); CMD=$(sym g_sdw_cmd); ACK=$(sym g_sdw_ack)
ERR=$(sym g_sdw_err); STATE=$(sym g_sdw_state); WROTE=$(sym g_sdw_written)
for v in GO CMD ACK ERR STATE WROTE; do
    [ -n "${!v}" ] || { echo "ELF 裡找不到 g_sdw_* 符號，韌體版本不對"; exit 1; }
done

BUF=0x91000000
CHUNK=$((16 * 1024 * 1024))
SIZE=$(stat -c %s "$SRC")
NCHUNK=$(( (SIZE + CHUNK - 1) / CHUNK ))

echo "來源   ：$SRC"
echo "大小   ：$SIZE bytes（$((SIZE / 1048576)) MB，分 $NCHUNK 塊）"
echo "目的   ：板子 SD 卡的 0:/video.bin"
echo "預估   ：約 $((SIZE / 895 / 1024 / 60 + 1)) 分鐘"
echo
echo "注意：這會建立或覆蓋 SD 卡上的 video.bin。其他檔案不受影響。"
if [ "$2" != "--yes" ]; then
    read -r -p "繼續？(yes/no) " ans
    [ "$ans" = "yes" ] || { echo "已取消"; exit 1; }
fi

conn() { "$CLI" -c port=SWD mode=HOTPLUG freq=24000 "$@" 2>&1; }

# CLI 印的是大寫十六進位，一律轉小寫再比對，免得 0000000A != 0000000a。
rd32() {
    conn -r32 "$1" 4 | grep -E "^0x[0-9A-Fa-f]{8} : " | awk '{print tolower($3)}' |
        head -1
}
# 注意 `|| true`：CLI 的 -w32 寫完會讀回來驗證，但韌體一看到指令就會把那個字
# 清成 0（那正是「做完了」的信號），於是驗證必然對不起來、CLI 回傳非零。
# 這裡不能讓 set -e 把腳本殺掉 —— 真正的成功與否是靠 wait_done/check_err 判斷的。
wr32() { conn -w32 "$1" "$2" >/dev/null || true; }

# 等韌體把 g_sdw_cmd 清成 0（代表指令做完）。
wait_done() {
    local i v
    for i in $(seq 1 "${1:-60}"); do
        v=$(rd32 "$CMD")
        [ "$v" = "00000000" ] && return 0
        sleep 1
    done
    echo "逾時：g_sdw_cmd 一直是 $v，state=$(rd32 "$STATE")"
    return 1
}

check_err() {
    local e; e=$(rd32 "$ERR")
    if [ "$e" != "00000000" ]; then
        echo "韌體回報 FatFs 錯誤 FRESULT=$((16#$e))（state=$(rd32 "$STATE")）"
        echo "  1=DISK_ERR 3=NOT_READY 4=NO_FILE 5=NO_PATH 9=INVALID_OBJECT"
        echo "  10=WRITE_PROTECTED 11=INVALID_DRIVE 12=NOT_ENABLED 13=NO_FILESYSTEM"
        exit 1
    fi
}

echo "==> 叫板子進入燒錄模式"
wr32 "$GO" 0x52574453          # 'SDWR'
sleep 2
ST=$(rd32 "$STATE")
if [ "$ST" != "0000000a" ]; then
    echo "板子沒有就緒（g_sdw_state=$ST，期望 0000000a）"
    echo "  1/2/3 = 卡在解鎖/掛接驅動/掛載"
    echo "  91    = 掛載失敗：沒插卡、卡沒格式化成 FAT32、或初始化失敗"
    echo "  FatFs 錯誤碼：$(rd32 "$ERR")"
    exit 1
fi
echo "  就緒"

echo "==> 查卡片空間"
wr32 "$CMD" 0x50000000
wait_done 30
check_err
TOTKB=$(( 16#$(rd32 "$(sym g_sdw_total_kb)") ))
FREEKB=$(( 16#$(rd32 "$(sym g_sdw_free_kb)") ))
echo "  容量 $((TOTKB / 1024)) MB，剩餘 $((FREEKB / 1024)) MB"
NEEDKB=$(( (SIZE + 1023) / 1024 ))
if [ "$FREEKB" -lt "$NEEDKB" ]; then
    echo "  空間不夠：需要 $((NEEDKB / 1024)) MB。請先騰出空間，或用 --duration 轉短一點。"
    wr32 "$CMD" 0x40000000
    exit 1
fi

echo "==> 開檔"
wr32 "$CMD" 0x10000000
wait_done
check_err

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
T0=$(date +%s)

for ((i = 0; i < NCHUNK; i++)); do
    dd if="$SRC" of="$TMP/c.bin" bs=1M skip=$((i * 16)) count=16 \
       status=none iflag=fullblock
    LEN=$(stat -c %s "$TMP/c.bin")

    conn -w "$TMP/c.bin" "$BUF" | grep -iE "^Error" && exit 1
    # 指令與長度合併成一個字寫入，避免「長度寫到一半就被讀走」的競爭。
    printf -v W '0x%08X' $(( (2 << 28) | LEN ))
    wr32 "$CMD" "$W"
    wait_done
    check_err

    DONE=$(( (i + 1) * CHUNK )); [ $DONE -gt "$SIZE" ] && DONE=$SIZE
    EL=$(( $(date +%s) - T0 ))
    echo "  第 $((i + 1))/$NCHUNK 塊　已寫 $((DONE / 1048576))/$((SIZE / 1048576)) MB　經過 ${EL}s"
done

echo "==> 關檔"
wr32 "$CMD" 0x30000000
wait_done
check_err

TOTAL=$(rd32 "$WROTE")
echo "  韌體回報已寫入 $((16#$TOTAL)) bytes（來源 $SIZE bytes）"
if [ "$((16#$TOTAL))" != "$SIZE" ]; then
    echo "  長度不符，卡可能滿了"
    exit 1
fi

echo "==> 重置回播放模式"
wr32 "$CMD" 0x40000000
sleep 3
echo "完成。板子應該已經開始播放。"
echo "檢查狀態：scripts/status.sh"
