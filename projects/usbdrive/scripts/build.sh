#!/usr/bin/env bash
#
# 建置隨身碟 app（ST 的 MSC_Standalone，經 tools/patch_project.py 改過）。
#
# 這支 app 沒有自己的原始碼目錄 —— 它就是 ST 的範例加上四項修改，
# 所以「同步原始碼」那一步換成「跑 patch」。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REPO=$(cd "$ROOT/../.." && pwd)

IDE="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/stm32cubeidec.exe"
CUBE="${CUBE_DIR:-$REPO/cube}"
PROJ="$CUBE/Projects/STM32H7S78-DK/Applications/USB_Device/MSC_Standalone"

[ -d "$PROJ" ] || {
    echo "找不到 MSC_Standalone：$PROJ"
    echo "請先跑任一專案的 scripts/setup.sh 取得 ST 韌體包"
    exit 1
}

echo "==> 套用修改"
CUBE_DIR="$CUBE" python "$ROOT/tools/patch_project.py"

echo "==> 編譯（CubeIDE headless）"
# headless 用自己的工作區，不要跟 GUI 或別的專案共用（board-notes 13.2）。
WS="${USBDRIVE_WS:-C:/STM32CubeIDE/ws_headless_msc}"
WSPATH=$(cygpath -w "$WS" 2>/dev/null || echo "$WS")
IMPORT=$(cygpath -w "$PROJ/STM32CubeIDE/Appli" 2>/dev/null || echo "$PROJ\STM32CubeIDE\Appli")

# 併成一行：續行的 \ 在 CRLF 的檔案裡會失效，而失效的症狀是 CubeIDE 在沒有
# -build 參數下啟動，匯入完就一直掛著不編譯也不結束（board-notes 13）。
"$IDE" --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data "$WSPATH" -import "$IMPORT" -build 'MSC_Standalone_Appli/Debug' 2>&1 | grep -iE "error|warning|Build Finished|Build Failed" || true
