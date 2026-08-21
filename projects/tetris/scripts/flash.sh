#!/usr/bin/env bash
# 燒錄到板子。Boot 進內部 Flash，遊戲進外部 Flash（需 external loader）。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# 韌體包與共用程式碼放在 repo 根目錄，所有專案共用。
REPO=$(cd "$ROOT/../.." && pwd)

CLI="C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.300.202508131133/tools/bin/STM32_Programmer_CLI.exe"
EL="$(dirname "$CLI")/ExternalLoader/MX66UW1G45G_STM32H7S78-DK.stldr"
PROJ="$REPO/cube/Projects/STM32H7S78-DK/Templates/Tetris"

# 首次燒錄需要 bootloader；之後只更新應用程式即可。
if [ "$1" = "--boot" ]; then
    echo "==> 燒錄 bootloader 到內部 Flash"
    "$CLI" -c port=SWD mode=UR -w "$PROJ/Binary/Boot_XIP.hex" -v
fi

echo "==> 燒錄遊戲到外部 Flash"
"$CLI" -c port=SWD mode=UR -el "$EL" \
    -w "$PROJ/STM32CubeIDE/Appli/Debug/Template_XIP_Appli.elf" -v

echo "==> 重置"
"$CLI" -c port=SWD mode=HOTPLUG -rst
