#!/usr/bin/env python3
"""把影片（或一疊 JPEG）轉成板子能播的影格包。

輸出的 .bin 可以走兩條路：
  - 燒進外部 Flash 的空白區（0x71000000），記憶體映射直接讀，適合短片
  - 放進 SD 卡當 `video.bin`，適合長片（外部 Flash 只有 128MB）

為什麼不是「放一堆 .jpg 進 SD 卡」：FatFs 每開一個檔案都要走目錄鏈，
7000 多個檔案的目錄本身就很大，而且每格多一次 f_open/f_close。
打包成單一檔案之後，播放時只要 f_lseek + f_read，位移表開機讀一次就好。

格式（小端序，'VFR2'）：
    magic     4   'VFR2'
    count     4   影格數
    width     4
    height    4
    fps_x100  4   素材格率 x100，韌體照這個節奏放
    max_size  4   最大單格 bytes，韌體用來確認緩衝區夠不夠
    表格      count x 8   每筆 (offset, size)，offset 從檔案開頭算起
    資料      連續的 JPEG，各自補齊到 4 位元組邊界

**補齊不是美觀考量**：輸入 DMA 的資料寬度是 word，每一塊的長度都必須是 4 的
倍數，補齊讓韌體那邊「往上取整」永遠讀得到有效記憶體（board-notes 16.12）。

用法：
    mp4pack.py <輸入.mp4 或影格目錄> <輸出.bin> [選項]

選項：
    --fps N         目的格率（預設 24）
    --quality N     JPEG 品質，ffmpeg 的 -q:v，2 最好 31 最差（預設 5）
    --rotate MODE   auto / cw / ccw / none（預設 auto：直式影片自動轉成橫式）
    --start S       從第幾秒開始
    --duration S    只取幾秒
    --keep-frames   保留中間產生的 JPEG，方便檢查
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b"VFR2"
HDR = 24
PANEL_W = 800
PANEL_H = 480


def probe(path):
    """讀出來源的寬高，用來決定要不要旋轉。"""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        capture_output=True, text=True, check=True).stdout.split()
    return int(out[0]), int(out[1])


def build_filter(src_w, src_h, rotate, fps):
    """組出 ffmpeg 的濾鏡鏈：旋轉 -> 等比縮放 -> 補黑邊 -> 定格率。

    面板是橫式 800x480，直式影片不轉的話只能縮成 270x480，兩側全是黑的
    （只用到 34% 的寬度）。轉 90 度之後可以填滿，代價是板子要側著看 ——
    這正是相簿那個顯示方向切換在做的事。
    """
    steps = []
    if rotate == "auto":
        rotate = "cw" if src_h > src_w else "none"
    if rotate == "cw":
        steps.append("transpose=1")     # 順時針 90 度，看的時候板子逆時針轉
    elif rotate == "ccw":
        steps.append("transpose=2")

    # force_original_aspect_ratio=decrease 保證整張都進得去（不裁切），
    # 再置中補黑邊填滿面板。相簿那邊「照片永不裁切」是同一個原則。
    steps.append(f"scale={PANEL_W}:{PANEL_H}:"
                 "force_original_aspect_ratio=decrease:flags=lanczos")
    steps.append(f"pad={PANEL_W}:{PANEL_H}:(ow-iw)/2:(oh-ih)/2:black")
    steps.append(f"fps={fps}")
    return ",".join(steps), rotate


def extract(src, outdir, args):
    src_w, src_h = probe(src)
    vf, rotate = build_filter(src_w, src_h, args.rotate, args.fps)
    print(f"  來源 {src_w}x{src_h}"
          f"{'（直式）' if src_h > src_w else ''}，旋轉：{rotate}")
    print(f"  濾鏡 {vf}")

    cmd = ["ffmpeg", "-v", "error", "-stats"]
    if args.start:
        cmd += ["-ss", str(args.start)]
    cmd += ["-i", src]
    if args.duration:
        cmd += ["-t", str(args.duration)]
    # yuvj420p 是每像素 1.5 bytes，解出來的 YCbCr 中間層只有 4:4:4 的一半，
    # 板子上的轉色時間直接砍半（board-notes 16.9）。
    cmd += ["-vf", vf, "-pix_fmt", "yuvj420p", "-q:v", str(args.quality),
            os.path.join(outdir, "f%06d.jpg")]
    subprocess.run(cmd, check=True)


def sof_dims(path):
    """從 SOF 取寬高，順便確認這是解得開的 baseline JPEG。"""
    b = open(path, "rb").read(4096)
    i = 2
    while i + 9 < len(b):
        if b[i] != 0xFF:
            return None
        m = b[i + 1]
        ln = (b[i + 2] << 8) | b[i + 3]
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            return ((b[i + 7] << 8) | b[i + 8], (b[i + 5] << 8) | b[i + 6])
        if m in (0xDA, 0xD9) or ln < 2:
            return None
        i += 2 + ln
    return None


def pack(framedir, dst, fps):
    files = sorted(f for f in os.listdir(framedir) if f.lower().endswith(".jpg"))
    if not files:
        print("找不到影格")
        return 1
    paths = [os.path.join(framedir, f) for f in files]

    dim = sof_dims(paths[0])
    if dim is None:
        print("第一張讀不到 SOF")
        return 1
    w, h = dim

    # 只讀大小不讀內容 —— 長片可以到幾百 MB，沒必要整包進記憶體。
    sizes = [os.path.getsize(p) for p in paths]
    table_end = HDR + len(paths) * 8
    off = (table_end + 3) & ~3
    table = []
    for s in sizes:
        table.append((off, s))
        off += (s + 3) & ~3

    with open(dst, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIIII", len(paths), w, h,
                            int(round(fps * 100)), max(sizes)))
        for o, s in table:
            f.write(struct.pack("<II", o, s))
        f.write(b"\0" * (((table_end + 3) & ~3) - table_end))
        for p, s in zip(paths, sizes):
            with open(p, "rb") as src:
                shutil.copyfileobj(src, f, 1 << 20)
            f.write(b"\0" * ((-s) & 3))

    total = os.path.getsize(dst)
    print(f"  {len(paths)} 格 {w}x{h} @ {fps}fps")
    print(f"  每格平均 {sum(sizes) // len(sizes)} bytes，最大 {max(sizes)} bytes")
    print(f"  {dst}：{total} bytes（{total / 1048576:.1f} MB）")
    if total > 126 * 1024 * 1024:
        print("  注意：超過外部 Flash 的可用空間，只能放 SD 卡")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="影片檔或已經切好的影格目錄")
    ap.add_argument("dst", help="輸出的 .bin")
    ap.add_argument("--fps", type=float, default=24)
    ap.add_argument("--quality", type=int, default=5)
    ap.add_argument("--rotate", choices=["auto", "cw", "ccw", "none"],
                    default="auto")
    ap.add_argument("--start", type=float, default=0)
    ap.add_argument("--duration", type=float, default=0)
    ap.add_argument("--keep-frames", action="store_true")
    args = ap.parse_args()

    if os.path.isdir(args.src):
        return pack(args.src, args.dst, args.fps)

    tmp = tempfile.mkdtemp(prefix="mp4pack_")
    try:
        print("==> 切影格")
        extract(args.src, tmp, args)
        print("==> 打包")
        rc = pack(tmp, args.dst, args.fps)
    finally:
        if args.keep_frames:
            print(f"  影格保留在 {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
