#!/usr/bin/env bash
# 電腦端參考渲染器：同一份原作程式碼、LovyanGFX、高解析度鏡像，用電腦的 g++ 編。
# 預設用 w64devkit（Windows）；其他平台把 CXX/CC 換成系統的 g++/gcc。
#
#   ./host/build_host.sh             # 編出 build/host/host_dash(.exe)
#   build/host/host_dash 1           # 畫 GT3，輸出 theme1.ppm 與 theme1_hi.ppm
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-/c/w64devkit/bin/g++}
CC=${CC:-/c/w64devkit/bin/gcc}
OUT=build/host
mkdir -p "$OUT"
FLAGS="-O1 -w -DLGFX_RACEDASH -I. -Icompat -Iorig -Ihires"

CXX_SRC="host/host_dash.cpp hires/hires.cpp orig/GT7DerivedMetrics.cpp orig/GT7UDPParser.cpp
lgfx/v1/LGFXBase.cpp lgfx/v1/LGFX_Sprite.cpp lgfx/v1/lgfx_fonts.cpp
lgfx/v1/misc/SpriteBuffer.cpp lgfx/v1/misc/common_function.cpp lgfx/v1/misc/pixelcopy.cpp
lgfx/v1/panel/Panel_Device.cpp"
C_SRC="lgfx/utility/lgfx_pngle.c lgfx/utility/lgfx_qoi.c lgfx/utility/lgfx_qrcode.c lgfx/utility/lgfx_tjpgd.c lgfx/utility/miniz.c"

# 標頭（LovyanGFX、原作、相容層、鏡像）任何一個比物件檔新就全部重編，簡單可靠。
NEWEST_H=$(ls -t lgfx/v1/*.hpp lgfx/v1/*/*.hpp hires/*.inc orig/*.h orig/dashboard/*.inc orig/dashboard/themes/*.inc compat/*.h compat/lgfx_user/*.hpp 2>/dev/null | head -1)
objs=""
for f in $CXX_SRC $C_SRC; do
    o="$OUT/$(echo "$f" | tr '/' '_' | sed 's/\.[a-z]*$/.o/')"
    objs="$objs $o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$NEWEST_H" -nt "$o" ]; then
        echo "  $f"
        case "$f" in
            *.c) "$CC" $FLAGS -std=gnu11 -c "$f" -o "$o" ;;
            *)   "$CXX" $FLAGS -std=gnu++17 -c "$f" -o "$o" ;;
        esac
    fi
done
"$CXX" -o "$OUT/host_dash" $objs
echo "==> $OUT/host_dash"
