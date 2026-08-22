#!/usr/bin/env python3
"""產生測試圖樣（或轉換真實照片）成 RGB888 原始檔。

檔名結尾固定是 _寬x高，run.sh 靠它決定要餵什麼尺寸給測試台。

圖樣挑的是最會照出重取樣缺陷的幾種：
  star  放射狀輻條 —— 由外往內細節連續變密，混疊一出現就看得到
  diag  數種角度的硬邊 —— 階梯狀鋸齒直接現形
  rings 同心圓 —— 曲線邊緣，真實照片裡最常見的鋸齒來源
真實照片看起來「還好」的缺陷，在這些圖樣上會被放大到無法忽視。
"""
import sys
import os
import numpy as np

# 1200x1800 直式：plan_geometry 會判定裁 10%、走「填滿」，
# 縮放比正好 2.25 倍，跟使用者實際在看的那批照片一致。
W, H = 1200, 1800


def save(out_dir, name, arr):
    """arr 是 (H, W, 3) 的 uint8。"""
    h, w = arr.shape[:2]
    path = os.path.join(out_dir, f"{name}_{w}x{h}.rgb")
    arr.astype(np.uint8).tofile(path)
    print(f"  {os.path.basename(path)}")


def gray(a):
    """(H, W) 灰階攤成三通道。"""
    return np.repeat(a[:, :, None], 3, axis=2)


def pat_star(w, h):
    yy, xx = np.mgrid[0:h, 0:w]
    cx, cy = w / 2.0, h / 2.0
    ang = np.arctan2(yy - cy, xx - cx)
    spokes = 64
    v = ((ang * spokes / (2 * np.pi)) % 1.0 < 0.5) * 255.0
    # 中心超出取樣極限，本來就必然混疊，塗掉免得誤導判讀。
    r = np.hypot(xx - cx, yy - cy)
    v[r < 90] = 128
    return gray(v)


def pat_diag(w, h):
    yy, xx = np.mgrid[0:h, 0:w]
    v = np.zeros((h, w), dtype=float)
    # 每條帶子一個角度，淺角度的階梯最明顯。
    for i, deg in enumerate([2, 5, 10, 20, 35, 45]):
        y0, y1 = i * h // 6, (i + 1) * h // 6
        band = slice(y0, y1)
        t = np.tan(np.deg2rad(deg))
        edge = (yy[band] - y0) < t * (xx[band] - w / 2.0) + (y1 - y0) / 2.0
        v[band] = edge * 255.0
    return gray(v)


def pat_rings(w, h):
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx - w / 2.0, yy - h / 2.0)
    # 由外往內周期漸縮，等效於連續掃過各種空間頻率。
    v = (np.sin(r * r / 2600.0) > 0) * 255.0
    return gray(v)


def from_photo(path, w, h):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    # 用高品質縮放產生「來源」，確保進入管線的資料本身沒有鋸齒，
    # 這樣看到的缺陷才能歸咎於被測的縮放程式碼。
    im = im.resize((w, h), Image.LANCZOS)
    return np.asarray(im)


def main():
    out_dir = sys.argv[1]
    photos = sys.argv[2:]

    print("  （圖樣尺寸 %dx%d，管線會縮到 480x800，比例 2.25）" % (W, H))
    save(out_dir, "star", pat_star(W, H))
    save(out_dir, "diag", pat_diag(W, H))
    save(out_dir, "rings", pat_rings(W, H))

    for p in photos:
        name = "photo_" + os.path.splitext(os.path.basename(p))[0]
        save(out_dir, name, from_photo(p, W, H))


if __name__ == "__main__":
    main()
