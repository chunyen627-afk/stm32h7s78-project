#!/usr/bin/env python3
"""Convert raw RGB565 framebuffer dumps into PNGs.

The dump is in physical landscape order (800x480). Rotating it back the way
the panel is held gives the 480x800 portrait view the player actually sees.
"""
import sys
import glob
from PIL import Image

PHYS_W, PHYS_H = 800, 480


def convert(path):
    with open(path, "rb") as f:
        data = f.read()
    expected = PHYS_W * PHYS_H * 2
    if len(data) != expected:
        print(f"{path}: expected {expected} bytes, got {len(data)}")
        return None

    img = Image.new("RGB", (PHYS_W, PHYS_H))
    px = img.load()
    for i in range(PHYS_W * PHYS_H):
        v = data[i * 2] | (data[i * 2 + 1] << 8)      # little-endian uint16
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5) & 0x3F) * 255 // 63
        b = (v & 0x1F) * 255 // 31
        px[i % PHYS_W, i // PHYS_W] = (r, g, b)

    # Undo the firmware's portrait->landscape mapping to recover the upright
    # image the player sees. gfx.c maps portrait (x,y) to physical
    # (y, GFX_W-1-x), which is the landscape buffer rotated anticlockwise;
    # rotating the dump clockwise puts it back upright.
    portrait = img.transpose(Image.Transpose.ROTATE_270)

    out = path.rsplit(".", 1)[0] + ".png"
    portrait.save(out)
    print(f"{out}  {portrait.width}x{portrait.height}")
    return out


def main():
    args = sys.argv[1:] or sorted(glob.glob("*.raw"))
    if not args:
        print("no .raw files given")
        return 1
    for path in args:
        convert(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
