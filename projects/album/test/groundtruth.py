#!/usr/bin/env python3
"""用超取樣算出測試圖樣「理論上正確」的 480x800 版本，當作評分基準。

先前的基準是 PIL 的線性光 Lanczos，但那把尺會獎勵銳利度 —— Mitchell 刻意
用銳利度換無 ringing，於是分數反而變差，等於用錯的尺否定對的方向。

這裡改成從連續函數直接求答案：圖樣本來就是解析式定義的，在每個目的像素
涵蓋的來源區域內取 N x N 個樣本平均（線性光空間），就是那個像素應有的值。
誰比較接近它，誰的重取樣就比較正確 —— 這跟「像不像某個特定演算法」無關。
"""
import sys
import numpy as np
from PIL import Image

SRC_W, SRC_H = 1200, 1800
SOX, CROP_W, CROP_H = 60, 1080, 1800
DST_W, DST_H = 480, 800
N = 6                      # 每個目的像素每軸的取樣數


def pattern_rings(x, y):
    r2 = (x - SRC_W / 2.0) ** 2 + (y - SRC_H / 2.0) ** 2
    return (np.sin(r2 / 2600.0) > 0) * 255.0


def pattern_star(x, y):
    ang = np.arctan2(y - SRC_H / 2.0, x - SRC_W / 2.0)
    v = ((ang * 64 / (2 * np.pi)) % 1.0 < 0.5) * 255.0
    r = np.hypot(x - SRC_W / 2.0, y - SRC_H / 2.0)
    return np.where(r < 90, 128.0, v)


def to_linear(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(l):
    l = np.clip(l, 0.0, 1.0)
    s = np.where(l <= 0.0031308, l * 12.92, 1.055 * np.power(l, 1 / 2.4) - 0.055)
    return np.clip(s * 255.0 + 0.5, 0, 255).astype(np.uint8)


def build(fn, name):
    sx = CROP_W / DST_W          # 每個目的像素涵蓋幾個來源像素
    sy = CROP_H / DST_H
    out = np.zeros((DST_H, DST_W))

    # 逐列處理，免得一次配 24M 個樣本的陣列
    off = (np.arange(N) + 0.5) / N
    xs = (SOX + (np.arange(DST_W)[:, None] + off[None, :]) * sx).ravel()
    for dy in range(DST_H):
        ys = (np.arange(dy, dy + 1)[:, None] + off[None, :]).ravel() * sy
        X, Y = np.meshgrid(xs, ys)
        v = to_linear(fn(X, Y))
        # 先攤成 (N, DST_W, N) 再對兩個取樣軸平均
        out[dy] = v.reshape(N, DST_W, N).mean(axis=(0, 2))

    rgb = np.repeat(to_srgb(out)[:, :, None], 3, axis=2)
    # 跟韌體一樣量化到 RGB565，比較條件才一致
    r, g, b = rgb[:, :, 0] & 0xF8, rgb[:, :, 1] & 0xFC, rgb[:, :, 2] & 0xF8
    q = np.dstack([r | (r >> 5), g | (g >> 6), b | (b >> 5)]).astype(np.uint8)
    Image.fromarray(q).save(f"out/{name}_1200x1800__truth.png")
    print(f"  {name}_1200x1800__truth.png")


if __name__ == "__main__":
    build(pattern_rings, "rings")
    build(pattern_star, "star")
