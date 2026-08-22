#!/usr/bin/env python3
"""把同一塊區域的各個變體並排成一張圖，並印出與參考組的數值差異。

並排是為了目視判讀（鋸齒是感知問題），數值是為了避免「看起來差不多」
這種主觀結論 —— 兩個都要，缺一個都容易下錯判斷。

用法：compare.py <out目錄> <圖樣名> <x> <y> <w> <h> <倍率> <變體...>
"""
import sys
import os
import numpy as np
from PIL import Image, ImageDraw

GAP = 8


def main():
    out_dir, pat = sys.argv[1], sys.argv[2]
    x, y, w, h, scale = map(int, sys.argv[3:8])
    variants = sys.argv[8:]

    tiles, arrays = [], {}
    for v in variants:
        p = os.path.join(out_dir, f"{pat}_1200x1800__{v}.png")
        im = Image.open(p).convert("RGB")
        arrays[v] = np.asarray(im).astype(np.int16)
        tiles.append((v, im.crop((x, y, x + w, y + h))
                        .resize((w * scale, h * scale), Image.NEAREST)))

    tw, th = tiles[0][1].size
    canvas = Image.new("RGB", (len(tiles) * (tw + GAP) - GAP, th + 18), "white")
    d = ImageDraw.Draw(canvas)
    for i, (name, im) in enumerate(tiles):
        canvas.paste(im, (i * (tw + GAP), 18))
        d.text((i * (tw + GAP) + 2, 4), name, fill="black")
    dst = os.path.join(out_dir, f"cmp_{pat}.png")
    canvas.save(dst)
    print(f"{dst}")

    # 以最後一個變體當參考，算平均絕對差。
    ref = variants[-1]
    print(f"  （與 {ref} 的平均絕對差）")
    for v in variants[:-1]:
        diff = np.abs(arrays[v] - arrays[ref]).mean()
        print(f"    {v:12s} {diff:6.2f}")


if __name__ == "__main__":
    main()
