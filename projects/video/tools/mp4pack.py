#!/usr/bin/env python3
"""把影片轉成 STM32H7S78-DK 相簿／播放器能跑的 .bin 影格包。

輸入吃什麼？**ffmpeg 讀得懂的都可以** —— mp4 / mkv / avi / mov / webm /
wmv / flv / ts / gif，甚至一個裝滿 JPEG 的資料夾。這支程式本身不解析影片，
只是把檔案交給 ffmpeg。

輸出是單一 .bin，複製到 SD 卡根目錄，相簿選單的「影片」就會列出來。

方向
----
面板是橫式 800x480。直式影片不轉的話只能縮成 270x480，兩側大片黑
（只用到 34% 的寬度）；轉 90 度可以填滿，代價是看的時候板子要側著擺。

**直式來源要 ccw，不是 cw。** 兩個方向都能填滿，但只有 ccw 跟板子實際的
擺放對得上 —— cw 轉出來跟相簿直立模式差 180 度，放上去是上下顛倒的
（實測踩過）。所以 auto 對直式選的是 **ccw**。

方向不對就自己指定：

    --rotate none    不轉
    --rotate cw      順時針 90 度
    --rotate ccw     逆時針 90 度
    --rotate 180     上下顛倒（來源被錄反時用）
    --flip           上下翻轉（鏡像，不是旋轉）
    --mirror         左右翻轉

**先用 --preview 看方向再轉整片。** 預覽只花幾秒，轉整片可能要好幾分鐘、
輸出好幾百 MB —— 沒必要轉完才發現躺反了。

檔案格式（小端序，VFR2）
------------------------
    magic     4   VFR2
    count     4   影格數
    width     4   必須是 800
    height    4   必須是 480
    fps_x100  4   素材格率 x100，韌體照這個節奏放
    max_size  4   最大單格 bytes，韌體用來確認緩衝區夠不夠
    表格      count x 8   每筆 (offset, size)
    資料      連續的 JPEG，各自補齊到 4 位元組邊界

**補齊不是美觀考量**：輸入 DMA 的資料寬度是 word，每一塊的長度都必須是
4 的倍數（board-notes 16.12，那個坑讓 26% 的影格解碼失敗）。

**這份是正本。** 使用者桌面上有一份一模一樣的複本（video2bin.py）方便隨手用，
改這裡的話記得同步過去。
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# 主控台可能是 CP950，中文訊息硬印會炸。用 replace 至少不會中斷。
try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

MAGIC = b"VFR2"
HDR = 24
PANEL_W = 800
PANEL_H = 480
FLASH_LIMIT = 126 * 1024 * 1024


def need(tool):
    if shutil.which(tool) is None:
        sys.exit("找不到 %s。請先安裝 ffmpeg 並加進 PATH：\n"
                 "    winget install Gyan.FFmpeg" % tool)


def probe(path):
    """讀出來源的寬高與格率。"""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,avg_frame_rate",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        capture_output=True, text=True)
    if out.returncode != 0 or len(out.stdout.split()) < 3:
        sys.exit("ffprobe 讀不出這個檔案的影像資訊：\n  %s\n%s"
                 % (path, out.stderr.strip()))
    w, h, rate = out.stdout.split()[:3]
    try:
        num, den = rate.split("/")
        fps = float(num) / float(den) if float(den) else 0.0
    except Exception:
        fps = 0.0
    return int(w), int(h), fps


def build_filter(src_w, src_h, rotate, flip, mirror, fps):
    """旋轉 -> 翻轉 -> 等比縮放 -> 補黑邊 -> 定格率。

    force_original_aspect_ratio=decrease 保證整張都進得去（不裁切），
    再置中補黑邊填滿面板 —— 跟相簿「照片永不裁切」同一個原則。
    """
    steps = []
    if rotate == "auto":
        # **直式來源要 ccw，不是 cw。**
        #
        # 兩個方向都能把直式填滿 800x480，但只有一個跟板子實際的擺放
        # 對得上：cw 轉出來的畫面跟相簿直立模式**差 180 度**，
        # 放上去是上下顛倒的（實測踩過，使用者手上能正常播的那部
        # VIDEO.BIN 就是 ccw 轉的）。
        #
        # 這不是美觀偏好，是硬體事實 —— 所以直接把它做進預設值，
        # 而不是只寫在文件裡等人去讀。
        rotate = "ccw" if src_h > src_w else "none"
    if rotate == "cw":
        steps.append("transpose=1")
    elif rotate == "ccw":
        steps.append("transpose=2")
    elif rotate == "180":
        steps.append("transpose=1,transpose=1")
    if flip:
        steps.append("vflip")
    if mirror:
        steps.append("hflip")
    steps.append("scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos"
                 % (PANEL_W, PANEL_H))
    steps.append("pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black" % (PANEL_W, PANEL_H))
    steps.append("fps=%s" % fps)
    return ",".join(steps), rotate


def preview(src, vf):
    """在影片的 10% / 50% / 90% 各抓一張 PNG，用來確認方向。

    轉整片要好幾分鐘、輸出好幾百 MB，方向錯了整包重來。先看三張。
    """
    dur = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", src],
        capture_output=True, text=True).stdout.strip()
    try:
        dur = float(dur)
    except Exception:
        dur = 0.0

    base = os.path.splitext(src)[0]
    made = []
    for pct in (0.1, 0.5, 0.9):
        out = "%s_preview_%d.png" % (base, int(pct * 100))
        cmd = ["ffmpeg", "-v", "error", "-y"]
        if dur > 0:
            cmd += ["-ss", str(dur * pct)]
        # 預覽不需要定格率，把濾鏡鏈最後那段 fps= 去掉
        cmd += ["-i", src, "-vf", vf.rsplit(",", 1)[0], "-frames:v", "1", out]
        if subprocess.run(cmd).returncode == 0 and os.path.exists(out):
            made.append(out)
    if not made:
        sys.exit("預覽失敗 —— ffmpeg 抓不到影格")
    print("  產生了：")
    for m in made:
        print("    %s" % m)
    print("")
    print("  打開來看方向對不對。")
    print("  不對就換 --rotate / --flip / --mirror 再預覽一次；")
    print("  對了就把 --preview 拿掉，跑正式轉檔。")


def extract(src, outdir, args, vf):
    cmd = ["ffmpeg", "-v", "error", "-stats"]
    if args.start:
        cmd += ["-ss", str(args.start)]
    cmd += ["-i", src]
    if args.duration:
        cmd += ["-t", str(args.duration)]
    # yuvj420p 每像素 1.5 bytes，解出來的 YCbCr 中間層只有 4:4:4 的一半，
    # 板子上的轉色時間直接砍半（board-notes 16.9）。
    cmd += ["-vf", vf, "-pix_fmt", "yuvj420p", "-q:v", str(args.quality),
            os.path.join(outdir, "f%06d.jpg")]
    if subprocess.run(cmd).returncode != 0:
        sys.exit("ffmpeg 切影格失敗")


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
        sys.exit("找不到影格")
    paths = [os.path.join(framedir, f) for f in files]

    dim = sof_dims(paths[0])
    if dim is None:
        sys.exit("第一張讀不到 SOF（不是 baseline JPEG？）")
    w, h = dim
    if (w, h) != (PANEL_W, PANEL_H):
        sys.exit("影格是 %dx%d，但韌體只接受 %dx%d" % (w, h, PANEL_W, PANEL_H))

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
            with open(p, "rb") as fp:
                shutil.copyfileobj(fp, f, 1 << 20)
            f.write(b"\0" * ((-s) & 3))

    total = os.path.getsize(dst)
    print("  %d 格 %dx%d @ %sfps" % (len(paths), w, h, fps))
    print("  每格平均 %d bytes，最大 %d bytes"
          % (sum(sizes) // len(sizes), max(sizes)))
    print("  %s：%d bytes（%.1f MB）" % (dst, total, total / 1048576))
    if max(sizes) > 1024 * 1024:
        print("  ** 單格超過 1MB，韌體緩衝區裝不下 —— 請調高 --quality 的數字 **")
    if total > FLASH_LIMIT:
        print("  注意：超過外部 Flash 的可用空間，只能放 SD 卡（複製到根目錄）")
    else:
        print("  可以放 SD 卡，也小到能燒進外部 Flash")


def main():
    ap = argparse.ArgumentParser(
        description="把影片轉成板子能跑的 .bin 影格包",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""範例：
  video2bin.py video.mp4                  輸出 video.bin，方向自動
  video2bin.py video.mp4 --preview        只產生三張預覽圖看方向
  video2bin.py video.mp4 --rotate 180     來源上下顛倒時
  video2bin.py video.mp4 --rotate none    不要轉，保持原本方向
  video2bin.py a.mkv out.bin --fps 30 --quality 4
  video2bin.py a.mov --start 60 --duration 30    只取第 60 秒起的 30 秒
""")
    ap.add_argument("src", help="影片檔（ffmpeg 讀得懂的都行）或影格目錄")
    ap.add_argument("dst", nargs="?", help="輸出的 .bin（預設同名同目錄）")
    ap.add_argument("--fps", type=float, default=24, help="目的格率（預設 24）")
    ap.add_argument("--quality", type=int, default=5,
                    help="JPEG 品質，2 最好 31 最差（預設 5）")
    ap.add_argument("--rotate", choices=["auto", "cw", "ccw", "180", "none"],
                    default="auto", help="旋轉（預設 auto：直式自動轉橫式）")
    ap.add_argument("--flip", action="store_true", help="上下翻轉（鏡像）")
    ap.add_argument("--mirror", action="store_true", help="左右翻轉（鏡像）")
    ap.add_argument("--start", type=float, default=0, help="從第幾秒開始")
    ap.add_argument("--duration", type=float, default=0, help="只取幾秒")
    ap.add_argument("--preview", action="store_true",
                    help="只產生三張預覽 PNG 確認方向，不轉檔")
    ap.add_argument("--keep-frames", action="store_true", help="保留中間的 JPEG")
    args = ap.parse_args()

    if not os.path.exists(args.src):
        sys.exit("找不到檔案：%s" % args.src)

    need("ffmpeg")
    need("ffprobe")

    dst = args.dst or (os.path.splitext(args.src)[0] + ".bin")

    if os.path.isdir(args.src):
        print("==> 打包（來源是影格目錄，不做旋轉縮放）")
        pack(args.src, dst, args.fps)
        return

    src_w, src_h, src_fps = probe(args.src)
    vf, rotate = build_filter(src_w, src_h, args.rotate, args.flip,
                              args.mirror, args.fps)

    print("==> 來源 %dx%d%s%s"
          % (src_w, src_h,
             "（直式）" if src_h > src_w else "（橫式）",
             "，%.2f fps" % src_fps if src_fps else ""))
    print("    旋轉 %s%s%s"
          % (rotate,
             "／上下翻轉" if args.flip else "",
             "／左右翻轉" if args.mirror else ""))
    print("    濾鏡 %s" % vf)

    if args.preview:
        print("==> 產生預覽（不轉檔）")
        preview(args.src, vf)
        return

    tmp = tempfile.mkdtemp(prefix="video2bin_")
    try:
        print("==> 切影格")
        extract(args.src, tmp, args, vf)
        print("==> 打包")
        pack(tmp, dst, args.fps)
    finally:
        if args.keep_frames:
            print("  影格保留在 %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
