#!/usr/bin/env bash
# 把原作儀表（C++）、LovyanGFX、相容層編成 libgt7dash.a，給 CubeIDE 的 C 專案連結。
# CubeIDE 專案是 C 專案，不會編 C++；用同一套 ARM 編譯器、同樣的 CPU 參數另外編，
# 再由 scripts/build.sh 從 make 命令列帶進去（USER_OBJS / LIBS），不必改專案檔。
#
# 輸出：gt7dash/build/libgt7dash.a。原始檔沒變就不重編（依時間戳）。
set -e
cd "$(dirname "$0")"
HERE=$(pwd)

GCC_BIN="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712/tools/bin"
CXX="$GCC_BIN/arm-none-eabi-g++"
CC="$GCC_BIN/arm-none-eabi-gcc"
AR="$GCC_BIN/arm-none-eabi-ar"

CPU="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard"
COMMON="$CPU -O2 -ffunction-sections -fdata-sections -w -DLGFX_RACEDASH -I. -Icompat -Iorig --specs=nano.specs"
CXXFLAGS="$COMMON -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit"
CFLAGS="$COMMON -std=gnu11"

OUT="$HERE/build"
mkdir -p "$OUT"

CXX_SRC="port.cpp hires/hires.cpp orig/GT7DerivedMetrics.cpp orig/GT7UDPParser.cpp
lgfx/v1/LGFXBase.cpp lgfx/v1/LGFX_Sprite.cpp lgfx/v1/lgfx_fonts.cpp
lgfx/v1/misc/SpriteBuffer.cpp lgfx/v1/misc/common_function.cpp lgfx/v1/misc/pixelcopy.cpp
lgfx/v1/panel/Panel_Device.cpp"
C_SRC="lgfx/utility/lgfx_pngle.c lgfx/utility/lgfx_qoi.c lgfx/utility/lgfx_qrcode.c lgfx/utility/lgfx_tjpgd.c lgfx/utility/miniz.c"

# port.cpp 把原作整個 include 進來，原作或相容層任何一個改了都要重編它。
DEPS_PORT=$(ls -t orig/*.h orig/dashboard/*.inc orig/dashboard/themes/*.inc compat/*.h compat/lgfx_user/*.hpp gt7dash.h 2>/dev/null | head -1)
# LovyanGFX 的標頭或高解析度鏡像的介面（hires/*.inc）改了，每個檔案都要重編。
DEPS_ALL=$(ls -t lgfx/v1/*.hpp lgfx/v1/*/*.hpp hires/*.inc hires/*.h 2>/dev/null | head -1)

objs=""
build() {   # build <編譯器> <旗標> <原始檔>
    local src="$3" obj="$OUT/$(echo "$3" | tr '/' '_' | sed 's/\.[a-z]*$/.o/')"
    objs="$objs $obj"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$DEPS_ALL" -nt "$obj" ] || { [ "$src" = port.cpp ] && [ "$DEPS_PORT" -nt "$obj" ]; }; then
        echo "  $src"
        $1 $2 -c "$src" -o "$obj"
    fi
}
echo "==> 編譯原作儀表與 LovyanGFX"
for f in $CXX_SRC; do build "$CXX" "$CXXFLAGS" "$f"; done
for f in $C_SRC;   do build "$CC"  "$CFLAGS"   "$f"; done

rm -f "$OUT/libgt7dash.a"
"$AR" rcs "$OUT/libgt7dash.a" $objs
ls -la "$OUT/libgt7dash.a" | awk '{print "  libgt7dash.a", $5, "bytes"}'
