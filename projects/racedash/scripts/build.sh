#!/usr/bin/env bash
# 編譯韌體。會先把 core/ 與 app_src/ 的最新版本同步進 CubeIDE 專案。
# 第一次（或改過 tools/patch_project.py 的原始碼清單之後）走 CubeIDE headless
# 產生 makefile；之後直接 make。要強制重新產生就設 FORCE_IDE=1。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REPO=$(cd "$ROOT/../.." && pwd)

IDE="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/stm32cubeidec.exe"
PROJ="$REPO/cube/Projects/STM32H7S78-DK/Templates/RaceDash"
DEBUG_DIR="$PROJ/STM32CubeIDE/Appli/Debug"
GCC_BIN="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin"

[ -d "$PROJ" ] || { echo "專案不存在，請先跑 scripts/setup.sh"; exit 1; }

echo "==> 原作儀表函式庫（C++）"
"$ROOT/gt7dash/build_lib.sh"
LIB="$ROOT/gt7dash/build/libgt7dash.a"
# 原作用到 C++ 標準函式庫；-u _printf_float 讓 newlib-nano 的 snprintf 印得出小數。
LINK_LIBS="-lstdc++ -lsupc++ -lm -u _printf_float"

echo "==> 同步原始碼"
cp "$ROOT"/app_src/*.c "$ROOT"/app_src/*.h "$ROOT"/gt7dash/gt7dash.h "$PROJ/Appli/Dash/"

if [ -f "$DEBUG_DIR/makefile" ] && [ -z "$FORCE_IDE" ]; then
    echo "==> 編譯（make）"
    PATH="$GCC_BIN:$PATH" make -C "$DEBUG_DIR" -j8 all USER_OBJS="$LIB" LIBS="$LINK_LIBS" 2>&1 | grep -iE "error|warning|No rule|Stop\.|\.elf|^ +[0-9]+ +[0-9]+" || true
    exit 0
fi

echo "==> 首次編譯（CubeIDE headless，順便產生 makefile）"
# -data 與 -import 必須用 Windows 反斜線路徑，否則 Eclipse 會把 C: 當 URI scheme。
# 用自己的 headless workspace，不要跟 GUI 共用（board-notes 13.2）。
WS=$(cygpath -w "C:/STM32CubeIDE/ws_headless_racedash")
IMPORT=$(cygpath -w "$PROJ/STM32CubeIDE/Appli")
"$IDE" --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data "$WS" -import "$IMPORT" -build 'RaceDash_Appli/Debug' 2>&1 | grep -iE "error|warning|Build Finished|Build Failed|^ +[0-9]+ +[0-9]+" || true
# CubeIDE 自己連結時不知道原作的函式庫，產生 makefile 之後再用 make 正式連結一次。
echo "==> 連結（make，帶原作函式庫）"
PATH="$GCC_BIN:$PATH" make -C "$DEBUG_DIR" -j8 all USER_OBJS="$LIB" LIBS="$LINK_LIBS" 2>&1 | grep -iE "error|warning|No rule|Stop\.|\.elf|^ +[0-9]+ +[0-9]+" || true
