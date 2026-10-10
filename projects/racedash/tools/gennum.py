#!/usr/bin/env python3
"""產生儀表的大數字字型（core/numfont.c / .h）。

用 Windows 內建的 Bahnschrift（DIN 風格，賽車儀表常見的字），預先渲染成
16 階灰度（4 bit）的點陣圖，所以邊緣是平滑的，不是方塊。
每種大小只收那個位置會用到的字，字型才不會大到沒必要。

每個大小的所有字共用同一個高度與基線（以數字 0 的上下緣對齊），
所以 "1:24.631" 這種字串直接左右接起來就對齊了。

    python tools/gennum.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT = r"C:\Windows\Fonts\bahnschrift.ttf"
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 名稱：(數字高度 px, 字體變化, 要收的字)
SIZES = {
    "xl": (200, "Bold",                   "0123456789NR-"),     # 檔位
    "l":  (104, "Bold",                   "0123456789-"),       # 車速、等待畫面
    "m":  (52,  "SemiBold SemiCondensed", "0123456789:.-"),     # 目前圈速
    "s":  (38,  "SemiBold SemiCondensed", "0123456789:.-/%"),   # 上一圈、最佳、轉速、油量、圈數
    "xs": (30,  "SemiBold SemiCondensed", "0123456789.-"),      # 可跑圈數
}


def font_for(height, variation):
    """找一個字級，讓數字 0 的實際高度剛好是 height。"""
    size = height
    for _ in range(30):
        f = ImageFont.truetype(FONT, size)
        f.set_variation_by_name(variation)
        top, bottom = f.getbbox("0")[1], f.getbbox("0")[3]
        h = bottom - top
        if h == height:
            return f, top
        size = max(4, round(size * height / h))
    return f, top


def render(ch, font, top, height):
    """把一個字畫進 寬 x height 的格子，回傳 (寬, 4bit 灰度位元組)。"""
    adv = int(round(font.getlength(ch)))
    l, _, r, _ = font.getbbox(ch)
    w = max(adv, r) + 1
    # 窄的標點（: .）左右留一點空，跟數字放一起才不會擠
    img = Image.new("L", (w, height), 0)
    ImageDraw.Draw(img).text((0, -top), ch, font=font, fill=255)
    # 去掉左右多餘的空白欄，但保留字距：最少留 adv 寬
    px = img.load()
    cols = [x for x in range(w) if any(px[x, y] for y in range(height))]
    if cols and ch not in ":./%":
        left = min(cols[0], max(0, l))
    else:
        left = 0
    width = max(adv - left, (cols[-1] - left + 1) if cols else adv)
    if width % 2:
        width += 1                       # 兩個像素一個位元組，寬度湊成偶數
    out = bytearray()
    for y in range(height):
        for x in range(left, left + width, 2):
            a = px[x, y] if x < w else 0
            b = px[x + 1, y] if x + 1 < w else 0
            out.append(((a >> 4) << 4) | (b >> 4))
    return width, bytes(out)


def main():
    h_lines = [
        "/* tools/gennum.py 產生，不要手改。字體：Bahnschrift（Windows 內建）。 */",
        "#ifndef NUMFONT_H",
        "#define NUMFONT_H",
        "",
        "#include <stdint.h>",
        "",
        "/* 一個字：寬度與 4 bit 灰度點陣（每列 w/2 位元組，高位是左邊的像素）。 */",
        "typedef struct {",
        "    char           ch;",
        "    uint16_t       w;",
        "    const uint8_t *data;",
        "} nf_glyph_t;",
        "",
        "typedef struct {",
        "    uint16_t          h;       /* 整個字型共用的高度 */",
        "    uint16_t          count;",
        "    const nf_glyph_t *glyphs;",
        "} nf_font_t;",
        "",
    ]
    c_lines = ["/* tools/gennum.py 產生，不要手改。字體：Bahnschrift（Windows 內建）。 */",
               '#include "numfont.h"', ""]
    total = 0
    for name, (height, var, chars) in SIZES.items():
        font, top = font_for(height, var)
        glyphs = []
        for ch in chars:
            w, data = render(ch, font, top, height)
            sym = f"nf_{name}_{ord(ch):02x}"
            c_lines.append(f"static const uint8_t {sym}[] = {{ /* '{ch}' {w}x{height} */")
            for i in range(0, len(data), 24):
                c_lines.append("    " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
            c_lines.append("};")
            glyphs.append((ch, w, sym))
            total += len(data)
        c_lines.append(f"static const nf_glyph_t nf_{name}_glyphs[] = {{")
        for ch, w, sym in glyphs:
            esc = "\\'" if ch == "'" else ch
            c_lines.append(f"    {{ '{esc}', {w}, {sym} }},")
        c_lines.append("};")
        c_lines.append(f"const nf_font_t nf_{name} = {{ {height}, {len(glyphs)}, nf_{name}_glyphs }};")
        c_lines.append("")
        h_lines.append(f"extern const nf_font_t nf_{name};   /* {height}px {var}：{chars} */")
        print(f"{name:3s} {height:4d}px {var:24s} {len(chars):2d} 字")
    h_lines += ["", "#endif", ""]
    with open(os.path.join(HERE, "core", "numfont.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(h_lines))
    with open(os.path.join(HERE, "core", "numfont.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(c_lines))
    print(f"總共 {total / 1024:.0f} KB")


if __name__ == "__main__":
    main()
