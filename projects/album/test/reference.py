#!/usr/bin/env python3
"""產生各種「正確做法」的對照組。

  refbox        sRGB 空間的面積平均（PIL BOX）
  reflanczos    sRGB 空間的 Lanczos-3
  reflinbox     線性光空間的面積平均
  reflin        線性光空間的 Lanczos-3  ← 品質天花板，拿來當比較基準

為什麼要有線性光版本：重取樣是加權平均，而平均只在線性光空間才有物理意義。
sRGB 是感知編碼，直接對編碼值平均會低估亮像素的光量，誤差集中在邊緣。

比較條件對齊：同一塊來源裁切區、同樣量化到 RGB565（不加抖動，
抖動只影響平坦漸層，不影響邊緣幾何）。

用法：reference.py <out 目錄>
"""
import sys
import os
import glob
import numpy as np
from PIL import Image

SRC_W, SRC_H = 1200, 1800
SOX, SOY, CROP_W, CROP_H = 60, 0, 1080, 1800   # plan_geometry 算出來的取用範圍
DST_W, DST_H = 480, 800


def to_linear(a):
    c = a.astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(l):
    l = np.clip(l, 0.0, 1.0)
    s = np.where(l <= 0.0031308, l * 12.92, 1.055 * np.power(l, 1 / 2.4) - 0.055)
    return np.clip(s * 255.0 + 0.5, 0, 255).astype(np.uint8)


def resize_channels(arr, filt):
    """arr 是 (H, W, 3) 的 float64，逐通道用 PIL 的 F 模式縮放。"""
    out = []
    for ch in range(3):
        im = Image.fromarray(arr[:, :, ch].astype(np.float32), mode="F")
        out.append(np.asarray(im.resize((DST_W, DST_H), filt)))
    return np.dstack(out).astype(np.float64)


def quant565(a):
    """截斷到 RGB565 再還原成 8 位元，跟韌體的量化一致（不含抖動）。"""
    r, g, b = (a[:, :, 0] & 0xF8), (a[:, :, 1] & 0xFC), (a[:, :, 2] & 0xF8)
    return np.dstack([r | (r >> 5), g | (g >> 6), b | (b >> 5)]).astype(np.uint8)


def main():
    out_dir = sys.argv[1]
    for raw in sorted(glob.glob(os.path.join(out_dir, "*.rgb"))):
        name = os.path.basename(raw).rsplit("_", 1)[0]
        a = np.fromfile(raw, dtype=np.uint8).reshape(SRC_H, SRC_W, 3)
        crop = a[SOY:SOY + CROP_H, SOX:SOX + CROP_W]

        jobs = [
            ("refbox",     Image.BOX,     False),
            ("reflanczos", Image.LANCZOS, False),
            ("reflinbox",  Image.BOX,     True),
            ("reflin",     Image.LANCZOS, True),
        ]
        for tag, filt, linear in jobs:
            if linear:
                res = resize_channels(to_linear(crop), filt)
                arr = quant565(to_srgb(res))
            else:
                res = resize_channels(crop.astype(np.float64), filt)
                arr = quant565(np.clip(res + 0.5, 0, 255).astype(np.uint8))
            path = os.path.join(out_dir, f"{name}_1200x1800__{tag}.png")
            Image.fromarray(arr).save(path)
            print(f"  {os.path.basename(path)}")


if __name__ == "__main__":
    main()
