#!/usr/bin/env bash
# 編譯韌體。會先把 core/ 與 app_src/ 的最新版本同步進 CubeIDE 專案。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

IDE="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/stm32cubeidec.exe"
PROJ="$ROOT/cube/Projects/STM32H7S78-DK/Templates/Tetris"

[ -d "$PROJ" ] || { echo "專案不存在，請先跑 scripts/setup.sh"; exit 1; }

echo "==> 同步原始碼"
cp "$ROOT"/core/*.c "$ROOT"/core/*.h "$ROOT"/app_src/game_main.c "$PROJ/Appli/Game/"

echo "==> 編譯（CubeIDE headless）"
# 注意：-data 與 -import 必須用 Windows 反斜線路徑，
# 否則 Eclipse 會把磁碟機代號當成 URI scheme 而失敗。
WSPATH=$(cygpath -w "$ROOT/ws" 2>/dev/null || echo "$ROOT\ws")
IMPORT=$(cygpath -w "$PROJ/STM32CubeIDE/Appli" 2>/dev/null || echo "$PROJ\STM32CubeIDE\Appli")

"$IDE" --launcher.suppressErrors -nosplash \
    -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
    -data "$WSPATH" -import "$IMPORT" \
    -build 'Template_XIP_Appli/Debug' 2>&1 |
    grep -iE "error|warning|Build Finished|Build Failed|^ +[0-9]+ +[0-9]+" || true
