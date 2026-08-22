#!/usr/bin/env bash
#
# 建立電子影片的 CubeIDE 專案。只需要執行一次。
#
# 韌體包（cube/）沿用 stm32h7s78-tetris 那份，不重複下載 540MB。
# 專案必須放在韌體包內的 Templates/ 層級，因為 .cproject 用相對路徑
# （../../../../../../..）指向 Drivers/ 與 Middlewares/，搬到別處會編不過。
#
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# 韌體包與共用程式碼放在 repo 根目錄，所有專案共用。
REPO=$(cd "$ROOT/../.." && pwd)

CUBE="${CUBE_DIR:-$ROOT/../../cube}"

if [ ! -d "$CUBE/Projects/STM32H7S78-DK" ]; then
    echo "找不到 STM32CubeH7RS 韌體包：$CUBE"
    echo "設定 CUBE_DIR 指到既有的 cube/ 目錄，或先跑 tetris 專案的 setup.sh"
    exit 1
fi
CUBE=$(cd "$CUBE" && pwd)
echo "==> 使用韌體包：$CUBE"

TPL="$CUBE/Projects/STM32H7S78-DK/Templates"
PROJ="$TPL/Video"
BSPEX="$CUBE/Projects/STM32H7S78-DK/Examples/BSP/Inc"
FATFS="$CUBE/Projects/STM32H7S78-DK/Applications/FatFs/FatFs_uSD_Standalone/Appli"

echo "==> 以 Template_XIP 為基底建立專案"
if [ ! -d "$PROJ" ]; then
    cp -r "$TPL/Template_XIP" "$PROJ"
fi

echo "==> 複製元件設定檔"
cp "$BSPEX/gt911_conf.h" "$BSPEX/mx66uw1g45g_conf.h" "$BSPEX/aps256xx_conf.h" "$PROJ/Appli/Inc/"

echo "==> 複製 FatFs 設定"
# 只拿設定檔。ST 的 fatfs.c 不用 —— 它的 MX_FATFS_Process 含 f_mkfs（格式化）
# 和寫測試檔的路徑。影片只需要開一個檔，掛載那幾行自己寫比較安全。
cp "$FATFS/Inc/ffconf.h" "$FATFS/Inc/sd_diskio_config.h" "$PROJ/Appli/Inc/"

echo "==> 記下韌體包位置供 build/flash 使用"
echo "$CUBE" > "$ROOT/.cube_path"

# 字型表沒進版控（6.4MB 的產生物），缺了就現場產生。

echo "==> 套用專案設定（HAL 模組、FatFs、原始碼、include 路徑）"
CUBE_DIR="$CUBE" python "$ROOT/tools/patch_project.py"

echo "==> 複製影片原始碼"
mkdir -p "$PROJ/Appli/Video"
cp "$ROOT"/app_src/*.c "$ROOT"/core/*.c "$PROJ/Appli/Video/"
cp "$REPO"/shared/xspi_psram.c "$PROJ/Appli/Video/"

echo
echo "完成。接著執行："
echo "  scripts/build.sh    編譯"
echo "  scripts/flash.sh    燒錄到板子"
