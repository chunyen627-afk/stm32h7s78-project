#!/usr/bin/env python3
"""把 800x480 RGB565 實體 framebuffer 轉成使用者實際看到的 480x800 直式 PNG。

gfx 的映射是 邏輯(x,y) -> 實體位移 (GFX_W-1-x)*PHYS_W + y，
也就是 phys[479-x][y] == portrait[y][x]，所以轉置之後左右翻即可。
方向弄反了畫面會上下顛倒卻不會報錯（board-notes 第七章那個教訓），
所以這裡把推導寫下來。
"""
import sys
import glob
import os
import numpy as np
from PIL import Image

PHYS_W, PHYS_H = 800, 480


def convert(path):
    data = np.fromfile(path, dtype="<u2")
    if data.size != PHYS_W * PHYS_H:
        print(f"{path}: 預期 {PHYS_W * PHYS_H} 個像素，讀到 {data.size}")
        return
    phys = data.reshape(PHYS_H, PHYS_W)

    r = ((phys >> 11) & 0x1F).astype(np.uint8)
    g = ((phys >> 5) & 0x3F).astype(np.uint8)
    b = (phys & 0x1F).astype(np.uint8)
    # 位元複製補回 8 位元，比左移補零準（255 才會是 255）。
    rgb = np.dstack([(r << 3) | (r >> 2),
                     (g << 2) | (g >> 4),
                     (b << 3) | (b >> 2)])

    portrait = rgb.transpose(1, 0, 2)[:, ::-1]
    out = os.path.splitext(path)[0] + ".png"
    Image.fromarray(portrait).save(out)
    print(f"  {os.path.basename(out)}")


def main():
    for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.fb"))):
        convert(p)


if __name__ == "__main__":
    main()
