#!/usr/bin/env bash
# 在 PC 上跑相簿的縮放與後製，輸出 PNG 供目視比較。
#
#   ./run.sh                 用內建測試圖樣
#   ./run.sh photo.jpg ...   另外加上真實照片
#
# VARIANTS 裡每一組是「名稱:編譯旗標」，會各編一個執行檔跑同一批輸入，
# 輸出檔名帶上變體名，方便並排比較。一次只改一個變數才分得出誰造成什麼。
set -e
cd "$(dirname "$0")"

REPO=$(cd ../../.. && pwd)
OUT=out
mkdir -p "$OUT"

# 每組一個編譯旗標，用來隔離單一變因。
VARIANTS=(
    "base:"                          # 面積平均 + 線性光 + 銳化 100
    "srgbavg:-DRESAMPLE_LINEAR=0"    # 隔離線性光的貢獻
)

echo "==> 產生測試圖"
python make_testimg.py "$OUT" "$@"

for v in "${VARIANTS[@]}"; do
    name=${v%%:*}
    flags=${v#*:}
    echo "==> 編譯並執行變體：$name ${flags:-（無額外旗標）}"
    gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
        $flags \
        -I stubs -I ../app_src -I ../core -I "$REPO/shared" \
        -o "$OUT/rs_$name.exe" \
        resample_shot.c "$REPO/shared/gfx.c" ../core/font_zh.c

    for raw in "$OUT"/*.rgb; do
        base=$(basename "$raw" .rgb)
        dims=${base##*_}                 # 檔名結尾是 _寬x高
        w=${dims%x*}
        h=${dims#*x}
        "$OUT/rs_$name.exe" "$raw" "$w" "$h" "$OUT/${base}__$name.fb"
    done
done

echo "==> 轉成 PNG"
python fb2png.py "$OUT"
echo "完成，看 $OUT/*.png"
