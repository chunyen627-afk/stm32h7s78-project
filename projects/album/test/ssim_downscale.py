#!/usr/bin/env python3
"""Öztireli & Gross (SIGGRAPH 2015) 的 SSIM 最佳化縮小，封閉解版本。

論文把縮小當成「讓輸出與輸入的 SSIM 最大」的最佳化問題，並推導出封閉解，
只要用到區塊平均與小卷積，成本與線性濾波同級 —— 這是少數在 MCU 上跑得動的
研究級演算法，所以拿它當代表來驗證「保細節類演算法對鋸齒有沒有幫助」。

流程（在線性光空間做，跟韌體一致）：
    L   = 區塊平均縮小（就是面積平均，也是我們韌體現在的做法）
    l2  = 對 I^2 做同樣的區塊平均
    mu  = 對 L 做小卷積
    sL2 = conv(L^2) - mu^2      輸出的局部變異數
    sI2 = conv(l2)  - mu^2      輸入的局部變異數（降到輸出網格上）
    S   = sqrt(sI2 / sL2)       把局部對比拉回原圖的水準
    D   = mu + S * (L - mu)

換句話說它是**自適應的局部對比補強**：縮小會壓低局部變異數，這個方法把它
補回去。平坦區 S 接近 1（不動），有細節的地方 S > 1（加強）。

用法：ssim_downscale.py <in.rgb> <寬> <高> <out.png>
"""
import sys
import numpy as np
from PIL import Image

DST_W, DST_H = 480, 800
MAX_CROP_PCT = 25


def to_linear(a):
    c = a.astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(l):
    l = np.clip(l, 0.0, 1.0)
    s = np.where(l <= 0.0031308, l * 12.92, 1.055 * np.power(l, 1 / 2.4) - 0.055)
    return np.clip(s * 255.0 + 0.5, 0, 255).astype(np.uint8)


def plan_geometry(sw, sh):
    """跟韌體 photo.c 的 plan_geometry() 同一套規則，否則比較的不是同一塊區域。"""
    wider = sw * DST_H > DST_W * sh
    if wider:
        used_w = sh * DST_W // DST_H
        crop = 100 - used_w * 100 // sw
    else:
        used_h = sw * DST_H // DST_W
        crop = 100 - used_h * 100 // sh
    if crop <= MAX_CROP_PCT:
        dw, dh = DST_W, DST_H
        if wider:
            src_h, src_w = sh, sh * DST_W // DST_H
        else:
            src_w, src_h = sw, sw * DST_H // DST_W
    else:
        src_w, src_h = sw, sh
        if wider:
            dw, dh = DST_W, sh * DST_W // sw
        else:
            dh, dw = DST_H, sw * DST_H // sh
    return dw, dh, src_w, src_h, (sw - src_w) // 2, (sh - src_h) // 2


def box_down(x, dw, dh):
    """區塊平均。來源尺寸不是輸出的整數倍時，用面積加權（跟韌體一致）。"""
    sh, sw = x.shape[:2]
    yi = (np.arange(sh) * dh // sh)
    xi = (np.arange(sw) * dw // sw)
    out = np.zeros((dh, dw) + x.shape[2:])
    cnt = np.zeros((dh, dw))
    np.add.at(out, (yi[:, None], xi[None, :]), x)
    np.add.at(cnt, (yi[:, None], xi[None, :]), 1)
    return out / np.maximum(cnt[..., None] if x.ndim == 3 else cnt, 1)


def conv3(x):
    """輸出網格上的 3x3 平均，論文裡的小卷積。"""
    p = np.pad(x, ((1, 1), (1, 1)) + ((0, 0),) * (x.ndim - 2), mode="edge")
    acc = np.zeros_like(x, dtype=np.float64)
    for dy in range(3):
        for dx in range(3):
            acc += p[dy:dy + x.shape[0], dx:dx + x.shape[1]]
    return acc / 9.0


def main():
    src, sw, sh, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    a = np.fromfile(src, dtype=np.uint8).reshape(sh, sw, 3)
    dw, dh, src_w, src_h, sox, soy = plan_geometry(sw, sh)
    crop = a[soy:soy + src_h, sox:sox + src_w]

    I = to_linear(crop)
    L = box_down(I, dw, dh)
    l2 = box_down(I * I, dw, dh)

    mu = conv3(L)
    sL2 = np.maximum(conv3(L * L) - mu * mu, 0.0)
    sI2 = np.maximum(conv3(l2) - mu * mu, 0.0)

    eps = 1e-8
    S = np.sqrt((sI2 + eps) / (sL2 + eps))
    # 平坦區不要放大雜訊：變異數太小就不動它（論文的作法）
    S = np.where(sI2 < 1e-5, 1.0, S)
    D = mu + S * (L - mu)

    out = np.zeros((DST_H, DST_W, 3), dtype=np.uint8)
    oy, ox = (DST_H - dh) // 2, (DST_W - dw) // 2
    out[oy:oy + dh, ox:ox + dw] = to_srgb(D)
    Image.fromarray(out).save(dst)
    print(f"  {dst}  ({src_w}x{src_h} -> {dw}x{dh})")


if __name__ == "__main__":
    main()
