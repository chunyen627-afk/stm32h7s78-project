#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""檢查照片能不能在 STM32H7S78-DK 的電子相簿上播，可順便修好。

**為什麼需要這支工具**：不能播的照片副檔名一樣是 .jpg、電腦上也打得開，
從外觀完全看不出差別。而在板子上的症狀是「畫面像卡住」—— 因為相簿在幾十
毫秒內一張張解碼失敗，螢幕上沒有東西可換，看起來就像當機。
2026-08-24 使用者丟了一個資料夾進去，66 張裡有 64 張是 progressive，就踩到了。

用法：
    python checkpic.py 資料夾或檔案 ...       只檢查
    python checkpic.py --fix 資料夾 ...        順便把 progressive 轉成 baseline

--fix 會**原地覆蓋**，轉檔前請確認原檔另有備份。
"""
import argparse
import os
import sys

# --- 相簿的限制（取自 projects/album/app_src/photo.c）------------------------
MAX_FILE_BYTES = 2 * 1024 * 1024      # JPEG_FILE_CAP
MAX_PIXELS     = 10 * 1024 * 1024 // 3  # RGB_CAP / 3 bytes per pixel
MAX_WIDTH      = 3413                 # 分帶緩衝 160KB，4:2:0 的 MCU 高 16

SOF_NAMES = {
    0xC0: ("baseline", True),
    0xC1: ("extended sequential", False),
    0xC2: ("progressive", False),
    0xC3: ("lossless", False),
    0xC5: ("differential sequential", False),
    0xC6: ("differential progressive", False),
    0xC7: ("differential lossless", False),
    0xC9: ("arithmetic sequential", False),
    0xCA: ("arithmetic progressive", False),
    0xCB: ("arithmetic lossless", False),
}


def scan_jpeg(path):
    """回傳 (sof_marker, 寬, 高, 分量數)。不是 JPEG 就回 (None, 0, 0, 0)。"""
    with open(path, "rb") as f:
        b = f.read(1 << 20)          # 標頭一定在前面，不必整檔讀進來

    if b[:2] != b"\xff\xd8":
        return None, 0, 0, 0

    i = 2
    while i + 9 < len(b):
        if b[i] != 0xFF:
            i += 1
            continue
        m = b[i + 1]
        if m in SOF_NAMES:
            h = (b[i + 5] << 8) | b[i + 6]
            w = (b[i + 7] << 8) | b[i + 8]
            return m, w, h, b[i + 9]
        if m == 0xD8 or m == 0x01 or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if m == 0xDA:                # 進到影像資料就沒有 SOF 了
            break
        i += 2 + ((b[i + 2] << 8) | b[i + 3])
    return None, 0, 0, 0


def judge(path):
    """回傳 (可不可以播, 原因, 詳細資訊)。"""
    size = os.path.getsize(path)
    m, w, h, comps = scan_jpeg(path)

    if m is None:
        return False, "不是 JPEG（副檔名可能被改過）", ""

    name, ok = SOF_NAMES[m]
    info = "%dx%d %s %d 分量 %.0fKB" % (w, h, name, comps, size / 1024)

    if not ok:
        return False, "是 %s JPEG，硬體只吃 baseline" % name, info
    if comps == 4:
        return False, "CMYK（4 分量），硬體不支援", info
    if size > MAX_FILE_BYTES:
        return False, "檔案 %.1fMB 超過 2MB 上限" % (size / 1048576.0), info
    if w > MAX_WIDTH:
        return False, "寬 %d 超過 %d" % (w, MAX_WIDTH), info
    if w * h > MAX_PIXELS:
        return False, "%.1f 百萬像素超過 %.1f 上限" % (w * h / 1e6, MAX_PIXELS / 1e6), info
    return True, "", info


def to_baseline(path):
    """原地轉成 baseline。回傳有沒有成功。"""
    try:
        from PIL import Image
    except ImportError:
        print("  !! 需要 Pillow 才能轉檔：pip install pillow")
        return False

    try:
        im = Image.open(path)
        if im.mode not in ("RGB", "L"):
            im = im.convert("RGB")
        # progressive=False 就是 baseline。quality 95 對已經壓過的圖夠用，
        # 再高只會讓檔案變大而看不出差別。
        im.save(path, "JPEG", quality=95, progressive=False, optimize=True)
        return True
    except Exception as e:
        print("  !! 轉檔失敗：%s" % e)
        return False


def collect(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in sorted(files):
                    if f.lower().endswith((".jpg", ".jpeg")):
                        out.append(os.path.join(root, f))
        elif os.path.isfile(p):
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(
        description="檢查照片能不能在電子相簿上播")
    ap.add_argument("src", nargs="*", help="資料夾或檔案")
    ap.add_argument("--fix", action="store_true",
                    help="把 progressive 轉成 baseline（原地覆蓋）")
    args = ap.parse_args()

    if not args.src:
        print()
        print("  把要檢查的資料夾或照片拖到這個視窗上，然後按 Enter。")
        print()
        print("  這支工具會告訴你哪些照片板子播不了、以及為什麼 ——")
        print("  不能播的照片副檔名一樣是 .jpg，從外觀看不出來，")
        print("  但放到板子上會讓畫面看起來像卡住。")
        print()
        return 0

    files = collect(args.src)
    if not files:
        print("沒有找到 .jpg / .jpeg")
        return 1

    bad = []
    for p in files:
        ok, why, info = judge(p)
        if not ok:
            bad.append((p, why, info))

    print()
    print("共 %d 張，其中 %d 張板子播不了。" % (len(files), len(bad)))

    if not bad:
        print("全部可以播。")
        return 0

    # 同樣的原因合併顯示，不要洗版
    from collections import Counter
    reasons = Counter(w for _p, w, _i in bad)
    print()
    for why, n in reasons.most_common():
        print("  %d 張：%s" % (n, why))
    print()
    print("例如：")
    for p, why, info in bad[:3]:
        print("  %s" % os.path.basename(p))
        print("      %s" % info)

    if not args.fix:
        print()
        print("加上 --fix 可以把 progressive 的自動轉成 baseline（原地覆蓋）。")
        print("其他原因（太大、CMYK、不是 JPEG）要自己處理。")
        return 1

    print()
    print("開始轉檔…")
    fixed = 0
    for p, why, _info in bad:
        if "progressive" not in why and "extended" not in why:
            continue          # 只有這兩種轉得動
        if to_baseline(p):
            fixed += 1

    print("轉好 %d 張。" % fixed)
    still = sum(1 for p in files if not judge(p)[0])
    if still:
        print("還有 %d 張仍然播不了（原因不是 progressive）。" % still)
    return 0


if __name__ == "__main__":
    sys.exit(main())
