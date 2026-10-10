#!/usr/bin/env bash
#
# 建立賽車儀表的 CubeIDE 專案。只需要執行一次。
# 韌體包（repo 根目錄的 cube/）沿用其他專案那份，沒有的話先跑 tetris 的 setup.sh。
# 專案必須放在韌體包內的 Templates/ 層級，因為 .cproject 用相對路徑
# （../../../../../../..）指向 Drivers/ 與 Middlewares/，搬到別處會編不過。
#
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REPO=$(cd "$ROOT/../.." && pwd)
CUBE="$REPO/cube"

[ -d "$CUBE/Projects/STM32H7S78-DK" ] || { echo "找不到韌體包 $CUBE，先跑 projects/tetris/scripts/setup.sh"; exit 1; }

TPL="$CUBE/Projects/STM32H7S78-DK/Templates"
PROJ="$TPL/RaceDash"
BSPEX="$CUBE/Projects/STM32H7S78-DK/Examples/BSP/Inc"

echo "==> 以 Template_XIP 為基底建立專案"
[ -d "$PROJ" ] || cp -r "$TPL/Template_XIP" "$PROJ"

echo "==> 複製元件設定檔"
cp "$BSPEX/gt911_conf.h" "$BSPEX/mx66uw1g45g_conf.h" "$BSPEX/aps256xx_conf.h" "$PROJ/Appli/Inc/"

echo "==> 套用專案設定（改名、HAL 模組、原始碼、include 路徑）"
python "$ROOT/tools/patch_project.py"

echo "==> 複製儀表原始碼"
mkdir -p "$PROJ/Appli/Dash"
cp "$ROOT"/app_src/*.c "$ROOT"/app_src/*.h "$ROOT"/gt7dash/gt7dash.h "$PROJ/Appli/Dash/"

echo
echo "完成。接著執行 scripts/build.sh 編譯、scripts/flash.sh 燒錄。"
