#!/usr/bin/env python3
"""產生儀表用的平滑字型（core/numfont.c / .h）。

全部預先渲染成 16 階灰度（4 bit），邊緣是平滑的。四種：

  digits  數字用，Bahnschrift（Windows 內建，DIN 風格）。整個字型以數字 0 的
          上下緣對齊，所以 "1:24.631" 直接左右接起來就對齊
  text    英文標籤用（SPEED、Best Lap…）。格子涵蓋上伸部與下伸部，
          高度參數是大寫 H 的高度
  seg     七段液晶數字（Retro、Classic 主題）。不是字體，是用多邊形畫的
          六角形筆畫，放大四倍畫再縮小，所以邊緣平滑 —— 不是方塊
  pixel   像素字（Pocket 主題，Game Boy 風格）。小字級、不做抗鋸齒，
          執行時每個點放大成一格，故意要方塊感

每種大小只收那個位置會用到的字，字型才不會大到沒必要。

    python tools/gennum.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONTS = r"C:\Windows\Fonts"
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
DIGITS_SEG = "0123456789-:.+NR "

# 名稱：(種類, 高度 px, 字體檔, 字體變化, 要收的字)
SIZES = {
    # --- 數字（Bahnschrift）---
    "xl": ("digits", 200, "bahnschrift.ttf", "Bold", "0123456789NR-"),
    "l":  ("digits", 104, "bahnschrift.ttf", "Bold", "0123456789-:.%"),
    "m":  ("digits", 52, "bahnschrift.ttf", "SemiBold SemiCondensed", "0123456789:.-+/% "),
    "s":  ("digits", 38, "bahnschrift.ttf", "SemiBold SemiCondensed", "0123456789:.-+/% "),
    "xs": ("digits", 30, "bahnschrift.ttf", "SemiBold SemiCondensed", "0123456789.-+/% "),
    # --- 英文標籤（Bahnschrift）---
    "t1": ("text", 12, "bahnschrift.ttf", "SemiBold", ASCII),     # 很小的註記：6000 RPM、km/h
    "t2": ("text", 17, "bahnschrift.ttf", "SemiBold", ASCII),     # 框框裡的標籤
    "t3": ("text", 24, "bahnschrift.ttf", "SemiBold", ASCII),     # 比較顯眼的標籤
    # --- 等寬（Consolas Bold，Mono 主題）---
    "c2": ("text", 15, "consolab.ttf", None, ASCII),
    "c3": ("text", 26, "consolab.ttf", None, ASCII),
    # --- 七段液晶 ---
    "gx": ("seg", 190, None, None, DIGITS_SEG),
    "gl": ("seg", 96, None, None, DIGITS_SEG),
    "gm": ("seg", 46, None, None, DIGITS_SEG),
    "gs": ("seg", 30, None, None, DIGITS_SEG),
    # --- 像素（Pocket）：存的是小字，執行時放大 ---
    "px": ("pixel", 7, "consolab.ttf", None, ASCII),
}


def load(fontfile, size, variation):
    f = ImageFont.truetype(os.path.join(FONTS, fontfile), size)
    if variation:
        f.set_variation_by_name(variation)
    return f


def font_for(fontfile, variation, probe, height):
    """找一個字級，讓 probe 這個字的實際高度剛好是 height。"""
    size = height
    for _ in range(30):
        f = load(fontfile, size, variation)
        top, bottom = f.getbbox(probe)[1], f.getbbox(probe)[3]
        h = bottom - top
        if h == height:
            break
        size = max(4, round(size * height / h))
    return f


def pack(img, x0, width, height):
    """灰度圖 -> 4 bit，兩個像素一個位元組，高位是左邊。"""
    px = img.load()
    w = img.width
    out = bytearray()
    for y in range(height):
        for x in range(x0, x0 + width, 2):
            a = px[x, y] if x < w else 0
            b = px[x + 1, y] if x + 1 < w else 0
            out.append(((a >> 4) << 4) | (b >> 4))
    return bytes(out)


def render_font_glyph(ch, font, top, height, aa=True):
    adv = int(round(font.getlength(ch)))
    l, _, r, _ = font.getbbox(ch) if ch.strip() else (0, 0, adv, 0)
    w = max(adv, r) + 2
    img = Image.new("L", (w, height), 0)
    d = ImageDraw.Draw(img)
    if not aa:
        d.fontmode = "1"
    d.text((0, -top), ch, font=font, fill=255)
    px = img.load()
    cols = [x for x in range(w) if any(px[x, y] for y in range(height))]
    left = min(cols[0], max(0, l)) if (cols and ch not in ":./%' ") else 0
    width = max(adv - left, (cols[-1] - left + 1) if cols else adv, 1)
    if width % 2:
        width += 1
    return width, pack(img, left, width, height)


# --- 七段 ---------------------------------------------------------------
#  a
# f b
#  g
# e c
#  d
SEGS = {
    "0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc",
    "5": "afgcd", "6": "afgedc", "7": "abc", "8": "abcdefg", "9": "abcdfg",
    "-": "g", "N": "ceg", "R": "eg", "+": "g|",
}


def render_seg(ch, height):
    S = 4                                   # 放大四倍畫，縮小時就有抗鋸齒
    H = height * S
    t = H * 0.12                            # 筆畫粗細
    gap = H * 0.018                         # 筆畫之間的縫
    if ch == ":":
        W = H * 0.26
    elif ch == ".":
        W = H * 0.22
    elif ch == " ":
        W = H * 0.60
    else:
        W = H * 0.60
    cw = H * 0.50                           # 字本身的寬，其餘是字距
    img = Image.new("L", (int(W) + 1, H), 0)
    d = ImageDraw.Draw(img)

    def hseg(y):                            # 橫筆：兩端是尖的六角形
        x0, x1, h = t / 2 + gap, cw - t / 2 - gap, t / 2
        d.polygon([(x0, y), (x0 + h, y - h), (x1 - h, y - h), (x1, y),
                   (x1 - h, y + h), (x0 + h, y + h)], fill=255)

    def vseg(x, y0, y1):                    # 直筆
        y0, y1, h = y0 + gap, y1 - gap, t / 2
        d.polygon([(x, y0), (x + h, y0 + h), (x + h, y1 - h), (x, y1),
                   (x - h, y1 - h), (x - h, y0 + h)], fill=255)

    if ch == ":":
        r = t * 0.55
        for cy in (H * 0.32, H * 0.70):
            d.ellipse([W / 2 - r, cy - r, W / 2 + r, cy + r], fill=255)
    elif ch == ".":
        r = t * 0.55
        d.ellipse([W / 2 - r, H - t / 2 - r * 2 + t / 2, W / 2 + r, H - t / 2 + t / 2], fill=255)
    elif ch in SEGS:
        segs = SEGS[ch]
        top, mid, bot = t / 2, H / 2, H - t / 2
        if "a" in segs: hseg(top)
        if "g" in segs: hseg(mid)
        if "d" in segs: hseg(bot)
        if "f" in segs: vseg(t / 2, top, mid)
        if "b" in segs: vseg(cw - t / 2, top, mid)
        if "e" in segs: vseg(t / 2, mid, bot)
        if "c" in segs: vseg(cw - t / 2, mid, bot)
        if "|" in segs: vseg(cw / 2, H * 0.30, H * 0.70)   # '+' 的直筆
    small = img.resize((max(2, round(img.width / S)), height), Image.LANCZOS)
    width = small.width + (small.width % 2)
    return width, pack(small, 0, width, height)


def main():
    h_lines = [
        "/* tools/gennum.py 產生，不要手改。字體：Bahnschrift、Consolas（Windows 內建）。 */",
        "#ifndef NUMFONT_H", "#define NUMFONT_H", "", "#include <stdint.h>", "",
        "/* 一個字：寬度與 4 bit 灰度點陣（每列 w/2 位元組，高位是左邊的像素）。 */",
        "typedef struct {", "    char           ch;", "    uint16_t       w;",
        "    const uint8_t *data;", "} nf_glyph_t;", "",
        "typedef struct {",
        "    uint16_t          h;       /* 整個字型共用的高度（格子高） */",
        "    uint16_t          count;",
        "    const nf_glyph_t *glyphs;",
        "} nf_font_t;", "",
    ]
    c_lines = ["/* tools/gennum.py 產生，不要手改。 */", '#include "numfont.h"', ""]
    total = 0
    for name, (kind, height, fontfile, var, chars) in SIZES.items():
        glyphs = []
        if kind in ("digits", "text", "pixel"):
            probe = "0" if kind == "digits" else "H"
            font = font_for(fontfile, var, probe, height)
            if kind == "digits":
                top = font.getbbox("0")[1]
                cell = height
            else:
                # 格子要裝得下所有字的上伸與下伸部
                boxes = [font.getbbox(c) for c in chars if c.strip()]
                top = min(b[1] for b in boxes)
                cell = max(b[3] for b in boxes) - top
            for ch in chars:
                w, data = render_font_glyph(ch, font, top, cell, aa=(kind != "pixel"))
                glyphs.append((ch, w, data))
        else:
            cell = height
            for ch in chars:
                w, data = render_seg(ch, height)
                glyphs.append((ch, w, data))

        for ch, w, data in glyphs:
            sym = f"nf_{name}_{ord(ch):02x}"
            c_lines.append(f"static const uint8_t {sym}[] = {{ /* {ord(ch):#04x} {w}x{cell} */")
            for i in range(0, len(data), 24):
                c_lines.append("    " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
            c_lines.append("};")
            total += len(data)
        c_lines.append(f"static const nf_glyph_t nf_{name}_glyphs[] = {{")
        for ch, w, data in glyphs:
            lit = {"'": "\\'", "\\": "\\\\"}.get(ch, ch)
            c_lines.append(f"    {{ '{lit}', {w}, nf_{name}_{ord(ch):02x} }},")
        c_lines.append("};")
        c_lines.append(f"const nf_font_t nf_{name} = {{ {cell}, {len(glyphs)}, nf_{name}_glyphs }};")
        c_lines.append("")
        desc = chars if len(chars) < 20 else "ASCII"
        h_lines.append(f"extern const nf_font_t nf_{name};   /* {kind} {height}px（格子 {cell}）：{desc} */")
        print(f"{name:3s} {kind:6s} {height:4d}px 格子 {cell:4d}  {len(chars):3d} 字")
    h_lines += ["", "#endif", ""]
    with open(os.path.join(HERE, "core", "numfont.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(h_lines))
    with open(os.path.join(HERE, "core", "numfont.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(c_lines))
    print(f"總共 {total / 1024:.0f} KB")


if __name__ == "__main__":
    main()
