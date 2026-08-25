#!/usr/bin/env python3
"""把影片轉成 STM32H7S78-DK 相簿／播放器能跑的 .bin 影格包。

輸入吃什麼？**ffmpeg 讀得懂的都可以** —— mp4 / mkv / avi / mov / webm /
wmv / flv / ts / gif，甚至一個裝滿 JPEG 的資料夾。這支程式本身不解析影片，
只是把檔案交給 ffmpeg。

輸出是**兩個檔案**：

    NAME.bin    影格包（畫面）
    NAME.wav    音軌（48kHz / 16-bit / 立體聲）

兩個都複製到 SD 卡根目錄。**檔名必須一樣** —— 韌體是靠同名去配對的
（`MOVIE10.bin` 找 `MOVIE10.wav`），改名要兩個一起改。
來源沒有音軌就只會有 .bin，影片照樣能播，只是沒聲音。

音訊為什麼只能是 48kHz / 16-bit / 立體聲：板子的音訊時脈是 PLL3Q 分出來的
49.152MHz，那是 48k 這一族專用的數字，44.1k 湊不出來（會音高不準而且
對影片持續漂移）。所以這裡一律轉成 48k，不管來源是什麼。

**音訊與影像的時間範圍一定要一致。** --start / --duration 會同時套用到
兩邊 —— 只切其中一邊的話，同步從第一秒就錯了。

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

**這份是正本。** 使用者桌面的 `影像轉檔工具\` 有一份一模一樣的複本，
加上兩個拖放用的批次檔。改這裡的話記得同步過去。
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

# 韌體只吃這一組（見上面的說明）。每秒 192000 bytes，換算檔案大小用。
WAV_RATE  = 48000
WAV_CH    = 2
WAV_BYTES_PER_SEC = WAV_RATE * WAV_CH * 2


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


def has_audio(path):
    """來源有沒有音軌。沒有就只出 .bin，不要產生一個空的 wav ——
    韌體看到同名的 wav 就會去開它，空檔會變成「有音軌但沒聲音」。"""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0",
         "-show_entries", "stream=codec_type",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        capture_output=True, text=True)
    return out.returncode == 0 and "audio" in out.stdout


def extract_audio(src, dst, args):
    """抽音軌成 48kHz / 16-bit / 立體聲的 WAV。

    **-ss / -t 的位置與寫法要跟切影格那邊一模一樣**，否則兩邊會從不同的
    時間點開始，同步從第一秒就錯 —— 而那個症狀看起來會像「韌體的同步
    寫壞了」。
    """
    cmd = ["ffmpeg", "-v", "error", "-stats", "-y"]
    if args.start:
        cmd += ["-ss", str(args.start)]
    cmd += ["-i", src]
    if args.duration:
        cmd += ["-t", str(args.duration)]
    cmd += ["-vn",                      # 只要聲音
            "-map", "a:0",              # 有多條音軌時取第一條
            "-acodec", "pcm_s16le",
            "-ar", str(WAV_RATE),
            "-ac", str(WAV_CH),
            dst]
    if subprocess.run(cmd).returncode != 0:
        sys.exit("ffmpeg 抽音軌失敗")

    size = os.path.getsize(dst)
    secs = (size - 44) / float(WAV_BYTES_PER_SEC)
    print("  %s：%.1f MB，%d:%02d（48kHz 立體聲 16-bit）"
          % (dst, size / 1048576.0, int(secs) // 60, int(secs) % 60))
    return secs


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
    return len(paths)


def pick_output(base, taken):
    """BASE.bin -> BASE02.bin -> BASE03.bin ...，跳過已經存在或這輪用過的。

    編號從 02 開始而不是 01，因為第一個就叫 BASE.bin（使用者說的
    「先取第一個名稱，之後批量自己補編號」）。兩位數補零讓它在
    相簿的清單裡照字母排序也是對的。

    **.wav 也要一起避開。** 韌體靠同名配對，所以這兩個檔案是一組的 ——
    只檢查 .bin 的話，可能挑到一個 .bin 不存在、但 .wav 已經存在的編號，
    結果新影片配到舊影片的聲音。那個症狀會像「同步整個壞掉」。
    """
    cand = base + ".bin"
    n = 1
    while (cand in taken or os.path.exists(cand)
           or os.path.exists(cand[:-4] + ".wav")):
        n += 1
        if n > 99:
            sys.exit("同名的檔案已經有 99 個了，換一個名字")
        cand = "%s%02d.bin" % (base, n)
    return cand


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
  video2bin.py *.mp4 --name MOVIE                 批量：MOVIE.bin/.wav、MOVIE02...
  video2bin.py a.mp4 --audio-only --name MOVIE10  只補音軌（.bin 已經有了）
""")
    ap.add_argument("src", nargs="*",
                    help="影片檔（ffmpeg 讀得懂的都行）或影格目錄，可以給多個")
    ap.add_argument("--out", metavar="檔案",
                    help="指定輸出的 .bin（只能配一個來源，會直接覆寫）")
    ap.add_argument("--name", metavar="名稱",
                    help="輸出檔名的開頭；第一個是「名稱.bin」，"
                         "之後自動編號成 名稱02.bin、名稱03.bin…")
    ap.add_argument("--ask-name", action="store_true",
                    help="開始前問一次要用什麼檔名（給拖放的批次檔用）")
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
    ap.add_argument("--no-audio", action="store_true",
                    help="不要抽音軌（只出 .bin）")
    ap.add_argument("--audio-only", action="store_true",
                    help="只抽音軌（.bin 已經轉好、只想補聲音時用，快很多）")
    # 給拖放用的 .bat 在沒收到檔案時呼叫。訊息放這裡而不是 .bat 裡，
    # 是因為 cmd 用「解析當下」的字碼頁讀 .bat，中文會被切斷。
    ap.add_argument("--dropinfo", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--name-hint", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.dropinfo:
        print("")
        if args.audio_only:
            print("  把影片檔拖到這個 .bat 上面**只會抽出聲音**，不重轉畫面（快很多）。")
            print("  用在「.bin 已經在卡上、只是沒有聲音」的情況。")
            print("")
            print("  **檔名要跟卡上的 .bin 一樣** —— 韌體靠同名配對。")
            print("  它會先問你名字：給「MOVIE10」就產生 MOVIE10.wav。")
            print("  一次拖多個的話依序是 名稱.wav、名稱02.wav、名稱03.wav…")
            print("")
            print("  轉好的 .wav 複製到 SD 卡根目錄，跟 .bin 放一起。")
            print("")
            return
        print("  把影片檔拖到這個 .bat 上面就會轉檔，一次可以拖多個。")
        print("")
        print("  每部影片會產生**兩個**檔案：")
        print("    名稱.bin   畫面")
        print("    名稱.wav   聲音（48kHz 立體聲）")
        print("  **兩個都要複製到 SD 卡根目錄，而且檔名必須一樣** ——")
        print("  韌體是靠同名配對的，改名要兩個一起改。")
        print("  來源沒有聲音就只會有 .bin，影片照樣能播。")
        print("")
        if args.name_hint:
            print("  這一支會先問你要用什麼檔名：")
            print("    給「MOVIE」-> MOVIE.bin、MOVIE02.bin、MOVIE03.bin…")
            print("    直接 Enter  -> 沿用各自的原檔名")
            print("  編號會跳過已經存在的檔案，所以分幾次拖也會接著編下去。")
        else:
            print("  輸出檔名沿用原檔名（video2.mp4 -> video2.bin），")
            print("  放在影片旁邊。想自己取名就用「轉成BIN-自訂檔名.bat」。")
        print("")
        print("  方向自動處理：直式來源轉 ccw、橫向不轉 —— 兩種都在板子上驗過，")
        print("  平常不用給任何參數。")
        print("")
        print("  轉好的 .bin 與 .wav 複製到 SD 卡根目錄，相簿選單的「影片」")
        print("  就會列出來（清單最多列 16 部）。播放時點畫面可以調音量。")
        print("")
        print("  要調參數就開命令列： python video2bin.py --help")
        return

    if not args.src:
        ap.error("要給一個影片檔或影格目錄")

    # 舊版是 "video2bin.py 來源 輸出.bin"，現在來源可以有多個，
    # 那種寫法會被當成兩個來源。明講一聲，不要默默做錯事。
    if len(args.src) == 2 and args.src[1].lower().endswith(".bin"):
        sys.exit("要指定輸出檔名請用 --out，例如："
                 "  video2bin.py %s --out %s"
                 % (args.src[0], args.src[1]))
    if args.out and len(args.src) > 1:
        sys.exit("--out 只能配一個來源。多個來源請用 --name")

    for one in args.src:
        if not os.path.exists(one):
            sys.exit("找不到檔案：%s" % one)

    need("ffmpeg")
    need("ffprobe")

    base = args.name
    if args.ask_name and not base and not args.out:
        print("")
        print("  輸出檔名（直接按 Enter = 沿用各自的原檔名）")
        print("  給了名字的話：第一個是「名稱.bin」，之後自動編號 名稱02.bin…")
        try:
            base = input("  > ").strip()
        except EOFError:
            base = ""
        base = base or None

    taken = []
    for i, one in enumerate(args.src):
        if len(args.src) > 1:
            print("")
            print("======== [%d/%d] %s ========"
                  % (i + 1, len(args.src), os.path.basename(one)))
        if args.out:
            dst = args.out
        elif base and args.audio_only:
            # **只補音軌時不能跳號。** 一般轉檔會避開已經存在的檔名，
            # 但這裡的目的正好相反 —— 要對上那個已經存在的 .bin
            # （韌體靠同名配對）。跳號的話新的 wav 會配到別部影片。
            stem = os.path.join(os.path.dirname(one) or ".", base)
            dst = ("%s.bin" % stem) if i == 0 else ("%s%02d.bin" % (stem, i + 1))
        elif base:
            dst = pick_output(os.path.join(os.path.dirname(one) or ".", base),
                              taken)
        else:
            dst = os.path.splitext(one)[0] + ".bin"
        taken.append(dst)
        convert_one(one, dst, args)

    if len(args.src) > 1:
        print("")
        print("  共 %d 個檔案。注意相簿的影片清單最多列 16 部。" % len(args.src))


def convert_one(src, dst, args):
    wav = os.path.splitext(dst)[0] + ".wav"

    if os.path.isdir(src):
        print("==> 打包（來源是影格目錄，不做旋轉縮放）")
        pack(src, dst, args.fps)
        return

    if args.audio_only:
        if args.no_audio:
            sys.exit("--audio-only 跟 --no-audio 不能一起用")
        if not has_audio(src):
            sys.exit("這個來源沒有音軌：%s" % src)
        print("==> 只抽音軌")
        extract_audio(src, wav, args)
        return

    src_w, src_h, src_fps = probe(src)
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
        preview(src, vf)
        return

    tmp = tempfile.mkdtemp(prefix="video2bin_")
    frames = 0
    try:
        print("==> 切影格")
        extract(src, tmp, args, vf)
        print("==> 打包")
        frames = pack(tmp, dst, args.fps)
    finally:
        if args.keep_frames:
            print("  影格保留在 %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    if args.no_audio:
        return
    if not has_audio(src):
        print("==> 音軌：來源沒有音軌，只出 .bin（影片照樣能播，只是沒聲音）")
        return

    print("==> 抽音軌")
    asec = extract_audio(src, wav, args)

    # **把兩邊的長度擺在一起。** 這是唯一能在轉檔當下看出「音畫會不會
    # 對不上」的地方 —— 到了板子上才發現就得整包重轉。
    vsec = frames / float(args.fps) if frames else 0.0
    if vsec:
        diff = asec - vsec
        print("  影像 %d:%02d／音訊 %d:%02d，差 %+.2f 秒"
              % (int(vsec) // 60, int(vsec) % 60,
                 int(asec) // 60, int(asec) % 60, diff))
        if abs(diff) > 1.0:
            print("  ** 兩邊差超過一秒 —— 來源本身的音畫長度就不一致，")
            print("     或 --start/--duration 只切到其中一邊。板子上會對不準。 **")


if __name__ == "__main__":
    main()
