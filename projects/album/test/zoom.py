#!/usr/bin/env python3
"""裁一小塊並用最近鄰放大，讓單一像素看得見。

鋸齒是像素等級的現象，把 480x800 整張縮在畫面上看永遠看不出來 ——
一定要放大到一個像素變成一個方塊才能判讀邊緣的過渡是否平滑。

用法：zoom.py <in.png> <x> <y> <w> <h> <倍率> <out.png>
"""
import sys
from PIL import Image

src, x, y, w, h, scale, dst = (sys.argv[1], *map(int, sys.argv[2:7]), sys.argv[7])
im = Image.open(src).convert("RGB").crop((x, y, x + w, y + h))
im = im.resize((w * scale, h * scale), Image.NEAREST)
im.save(dst)
print(f"{dst}: 從 {src} 裁 ({x},{y}) {w}x{h}，放大 {scale} 倍")
