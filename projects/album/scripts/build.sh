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

# bootloader 也要建一次。patch_project.py 會在它的 SystemClock_Config 補開
# HSE，而隨身碟模式需要那個（USB HS 的 PHY 要 HSE）。沒有自建版的話
# flash.sh --boot 只能燒 Binary/ 底下的預編 hex，那顆沒有 HSE ——
# 症狀是相簿一切正常、USB 也會切換，但電腦完全看不到磁碟機，毫無錯誤訊息。
BOOT_ELF="$PROJ/STM32CubeIDE/Boot/Debug/Template_XIP_Boot.elf"
if [ ! -f "$BOOT_ELF" ] || [ -n "$FORCE_IDE" ]; then
    echo "==> 建置 bootloader（含 HSE）"
    BWS=$(cygpath -w "${ALBUM_WS:-C:/STM32CubeIDE/ws_headless_album}" 2>/dev/null || echo "C:/STM32CubeIDE/ws_headless_album")
    BIMP=$(cygpath -w "$PROJ/STM32CubeIDE/Boot" 2>/dev/null || echo "$PROJ\STM32CubeIDE\Boot")
    "$IDE" --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data "$BWS" -import "$BIMP" -build 'Template_XIP_Boot/Debug' 2>&1 | grep -iE "error|Build Finished|Build Failed" || true
fi

if [ -f "$DEBUG_DIR/makefile" ] && [ -z "$FORCE_IDE" ]; then
    echo "==> 編譯（make）"
    PATH="$GCC_BIN:$PATH" make -C "$DEBUG_DIR" -j8 all 2>&1 |
        grep -iE "error|warning|No rule|Stop\.|\.elf|^ +[0-9]+ +[0-9]+" || true
    exit 0
fi

echo "==> 首次編譯（CubeIDE headless，順便產生 makefile）"
# 注意：-data 與 -import 必須用 Windows 反斜線路徑，
# 否則 Eclipse 會把磁碟機代號當成 URI scheme 而失敗。
# headless 用自己的工作區，不要跟 GUI 的共用。
# 共用的話兩邊會互相留下 .lock 與未存檔狀態，下一次啟動卡在
# 「refreshing workspace to recover changes」很久，或是 -import 直接
# 撞上「already exists in the workspace!」而中止（board-notes 13.2）。
WS="${ALBUM_WS:-C:/STM32CubeIDE/ws_headless_album}"
WSPATH=$(cygpath -w "$WS" 2>/dev/null || echo "$WS")
IMPORT=$(cygpath -w "$PROJ/STM32CubeIDE/Appli" 2>/dev/null || echo "$PROJ\STM32CubeIDE\Appli")

# 併成一行：本 repo 的腳本在工作目錄是 CRLF，行末的 \ 會被  接走，而且這裡原本是 \（雙反斜線）。續行失效的話 CubeIDE 會在**沒有 -build 參數**下啟動，匯入完就一直掛著不編譯也不結束 —— 症狀是「編譯永遠不會回來」。
"$IDE" --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data "$WSPATH" -import "$IMPORT" -build 'Album_Appli/Debug' 2>&1 | grep -iE "error|warning|Build Finished|Build Failed|^ +[0-9]+ +[0-9]+" || true
