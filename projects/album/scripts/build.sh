#!/usr/bin/env bash
# 編譯韌體。會先把 core/ 與 app_src/ 的最新版本同步進 CubeIDE 專案。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# 韌體包與共用程式碼放在 repo 根目錄，所有專案共用。
REPO=$(cd "$ROOT/../.." && pwd)

IDE="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/stm32cubeidec.exe"
CUBE="${CUBE_DIR:-$REPO/cube}"
PROJ="$CUBE/Projects/STM32H7S78-DK/Templates/Album"

[ -d "$PROJ" ] || { echo "專案不存在，請先跑 scripts/setup.sh"; exit 1; }

echo "==> 同步原始碼"
cp "$ROOT"/app_src/*.c "$ROOT"/app_src/*.h  "$REPO"/shared/gfx.c "$REPO"/shared/gfx.h "$REPO"/shared/xspi_psram.c  "$PROJ/Appli/Album/"
cp "$ROOT"/core/*.c "$ROOT"/core/*.h "$PROJ/Appli/Album/" 2>/dev/null || true

# 有 makefile 就直接 make，快很多而且不用啟動整個 IDE。
# 第一次、或改過 tools/patch_project.py 的原始碼清單之後，makefile 需要
# 由 CubeIDE 重新產生 —— 這時把 Debug/ 刪掉，或設 FORCE_IDE=1。
DEBUG_DIR="$PROJ/STM32CubeIDE/Appli/Debug"
GCC_BIN="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin"

if [ -f "$DEBUG_DIR/makefile" ] && [ -z "$FORCE_IDE" ]; then
    echo "==> 編譯（make）"
    PATH="$GCC_BIN:$PATH" make -C "$DEBUG_DIR" -j8 all 2>&1 |
        grep -iE "error|warning|No rule|Stop\.|\.elf|^ +[0-9]+ +[0-9]+" || true
    exit 0
fi

echo "==> 首次編譯（CubeIDE headless，順便產生 makefile）"
# 注意：-data 與 -import 必須用 Windows 反斜線路徑，
# 否則 Eclipse 會把磁碟機代號當成 URI scheme 而失敗。
WS="${ALBUM_WS:-C:/STM32CubeIDE/workspace_2.0.0}"
WSPATH=$(cygpath -w "$WS" 2>/dev/null || echo "$WS")
IMPORT=$(cygpath -w "$PROJ/STM32CubeIDE/Appli" 2>/dev/null || echo "$PROJ\STM32CubeIDE\Appli")

"$IDE" --launcher.suppressErrors -nosplash \\
    -application org.eclipse.cdt.managedbuilder.core.headlessbuild \\
    -data "$WSPATH" -import "$IMPORT" \\
    -build 'Album_Appli/Debug' 2>&1 |\
    grep -iE "error|warning|Build Finished|Build Failed|^ +[0-9]+ +[0-9]+" || true
