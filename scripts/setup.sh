#!/usr/bin/env bash
#
# 取得 ST 官方韌體包，並把遊戲原始碼接進 CubeIDE 專案。
# 只需要執行一次；之後改完 core/ 直接跑 scripts/build.sh 即可。
#
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

echo "==> 下載 STM32CubeH7RS 韌體包（約 540MB，含 submodules）"
if [ ! -d cube ]; then
    git clone --depth 1 https://github.com/STMicroelectronics/STM32CubeH7RS.git cube
fi

echo "==> 取得 submodules（HAL / CMSIS / BSP / GT911 觸控驅動）"
(cd cube && git submodule update --init --depth 1 --recursive)

TPL="$ROOT/cube/Projects/STM32H7S78-DK/Templates"
PROJ="$TPL/Tetris"

echo "==> 以 Template_XIP 為基底建立專案"
# 專案必須放在韌體包內的這個層級，因為 .cproject 用相對路徑
# （../../../../../../..）指向 Drivers/ 與 Middlewares/。搬到別處會編不過。
if [ ! -d "$PROJ" ]; then
    cp -r "$TPL/Template_XIP" "$PROJ"
fi

echo "==> 複製元件設定檔"
# Template_XIP 本身不含這些，要從 BSP 範例借用。
cp "$ROOT/cube/Projects/STM32H7S78-DK/Examples/BSP/Inc/gt911_conf.h" \
   "$ROOT/cube/Projects/STM32H7S78-DK/Examples/BSP/Inc/mx66uw1g45g_conf.h" \
   "$ROOT/cube/Projects/STM32H7S78-DK/Examples/BSP/Inc/aps256xx_conf.h" \
   "$PROJ/Appli/Inc/"

echo "==> 套用專案設定（HAL 模組、原始碼、include 路徑）"
python "$ROOT/tools/patch_project.py"

echo "==> 複製遊戲原始碼"
mkdir -p "$PROJ/Appli/Game"
cp "$ROOT"/core/*.c "$ROOT"/core/*.h "$ROOT"/app_src/game_main.c "$PROJ/Appli/Game/"

echo
echo "完成。接著執行："
echo "  scripts/build.sh    編譯"
echo "  scripts/flash.sh    燒錄到板子"
