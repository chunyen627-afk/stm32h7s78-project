#!/usr/bin/env python3
"""把一疊 JPEG 影格打包成單一二進位檔，燒進外部 Flash 的空白區。

為什麼不編成 C 陣列：120 幀 x 67KB = 8MB，轉成十六進位文字會是四十幾 MB 的
原始碼，編譯慢得離譜。而外部 NOR Flash 是**記憶體映射**的（0x70000000），
把資料燒進去之後韌體可以直接就地讀取 —— 不用複製、不用解析容器、
連 SD 卡都不需要。128MB 的空間目前只用了 1.5MB。

格式（小端序）：
    magic   4   'VFRM'
    count   4   影格數
    width   4
    height  4
    表格    count x 8   每筆 (offset, size)，offset 從檔案開頭算起
    資料    連續的 JPEG，各自 4 位元組對齊

用法：packframes.py <影格目錄> <輸出.bin>
"""
import sys
import os
import glob
import struct

HDR = 16


def main():
    src, dst = sys.argv[1], sys.argv[2]
    files = sorted(glob.glob(os.path.join(src, "*.jpg")))
    if not files:
        print("找不到影格")
        return 1

    blobs = [open(f, "rb").read() for f in files]

    # 尺寸從第一張的 SOF 取，順便驗證所有幀一致 —— 不一致的話韌體那邊
    # 會用錯的緩衝區大小，症狀是畫面錯位而不是明確的錯誤。
    def sof_size(b):
        i = 2
        while i + 9 < len(b):
            if b[i] != 0xFF:
                break
            m = b[i + 1]
            ln = (b[i + 2] << 8) | b[i + 3]
            if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
                return ((b[i + 7] << 8) | b[i + 8],
                        (b[i + 5] << 8) | b[i + 6])
            if m in (0xDA, 0xD9) or ln < 2:
                break
            i += 2 + ln
        return None

    dim = sof_size(blobs[0])
    if dim is None:
        print("第一張讀不到 SOF")
        return 1
    for i, b in enumerate(blobs[1:], 1):
        if sof_size(b) != dim:
            print(f"第 {i} 張尺寸不一致：{sof_size(b)} vs {dim}")
            return 1
    w, h = dim

    table_end = HDR + len(blobs) * 8
    off = (table_end + 3) & ~3
    table, data = [], []
    for b in blobs:
        table.append((off, len(b)))
        pad = (-len(b)) & 3
        data.append(b + b"\0" * pad)
        off += len(b) + pad

    with open(dst, "wb") as f:
        f.write(b"VFRM")
        f.write(struct.pack("<III", len(blobs), w, h))
        for o, s in table:
            f.write(struct.pack("<II", o, s))
        f.write(b"\0" * ((table_end + 3 & ~3) - table_end))
        for d in data:
            f.write(d)

    total = os.path.getsize(dst)
    print(f"  {len(blobs)} 幀 {w}x{h}，每幀平均 {sum(len(b) for b in blobs)//len(blobs)} bytes")
    print(f"  {dst}：{total} bytes（{total/1048576:.2f} MB）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
