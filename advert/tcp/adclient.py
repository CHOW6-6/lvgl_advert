#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
adclient.py —— 云屏广告机 · 电脑端（WSL）上传客户端

把一段视频 / 一张图片打包成开发板能直接播的「广告包」，再通过 LADP 协议
推到 GEC6818 上。整个过程不需要在板上装任何东西，也不依赖 scp/sftp。

广告包的目录结构（也是板上 media/ 下的结构）：

    <id>/
        cover.jpg        704x340   正片 / 封面（播放页展示的就是它）
        thumb.jpg        232x130   缩略图（广告库网格用）
        f_000.jpg ...    704x340   视频帧序列（图片广告没有）
        meta.conf        一行：类型|标题|副标题|帧率|停留秒

为什么用「帧序列」而不是 mp4：
    6818 的 rootfs 里没有 ffmpeg、没有 SDL、没有硬件解码器，Linux 3.4 内核
    也没有 V4L2 硬解节点。要播 H.264 就得把整个 ffmpeg 交叉编译进去，
    光库就几十 MB，而且纯软件解码 800x480 在 A53 上跑不到 15fps。
    换成 JPEG 帧序列后：板上只需要 LVGL 自带的 tjpgd 解码器（已经在工程里），
    每帧独立、可随机跳转、传一半断了也不影响已传的帧，还能随手删几帧降帧率。

坑：JPEG 的色度采样必须是 4:2:0（yuvj420p）。
    ffmpeg 从 PNG 转 JPEG 时会默认保留全色度（4:4:4），文件是合法的，
    PC 上看着也正常，但 tjpgd 解 4:4:4 会静默失败 —— 表现是图片区域一片空白，
    连日志都没有。所以下面每个 -vf 后面都硬编码了 -pix_fmt yuvj420p。
    4:2:0 顺带还能把文件压小三成。

用法：
    ./adclient.py ping
    ./adclient.py list
    ./adclient.py push --id 01_watch --title "臻蓝系列" --sub "BLUE SERIES" \\
                       --video a.mp4 --cover a.png
    ./adclient.py rm --id 01_watch
    ./adclient.py ticker --text "公益广告 · 节约用水，人人有责"

    # 只打包不上传（产物留着用 scp / U 盘拷都行）
    ./adclient.py pack --id 01_watch --title "臻蓝系列" --video a.mp4 --out ./pkg
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import tempfile
import zlib

# ---- 与板上 advert_ui.c 的 CARD_W / CARD_H / THUMB_W / THUMB_H 严格对应 ----
CARD_W, CARD_H = 704, 312
THUMB_W, THUMB_H = 232, 130

DEFAULT_PORT = 9500
MAX_FRAMES = 480          # 帧数上限，超过就建议降帧率
MAX_FRAME_BYTES = 900 * 1024


# =====================================================================
# 协议：LADP/1
# =====================================================================
class LadpError(RuntimeError):
    pass


class LadpClient:
    """一条 TCP 连接上顺序发多条命令，服务端一条一条回 OK/ERR。"""

    def __init__(self, host, port=DEFAULT_PORT, timeout=15.0):
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.fp = self.sock.makefile("rb")
        self.banner = self.cmd_hello()
        self.sync_clock()

    # ---------- 校时 ----------
    def sync_clock(self):
        """把电脑的时间推给板子。

        6818 没有 RTC 电池，断电后系统时间会退回 1970/2015，而板上又没有外网
        可以走 NTP —— 界面右上角那个钟就完全是错的。最靠谱的授时源就是这台电脑，
        所以每次连上都顺手校一次，成本只有一条命令。
        失败不抛异常：老版本服务端没有 TIME 命令也不能影响正常传文件。"""
        try:
            self.cmd("TIME %d" % int(time.time()))
        except Exception:
            pass

    # ---------- 底层 ----------
    def _readline(self):
        line = self.fp.readline()
        if not line:
            raise LadpError("连接被服务端关闭")
        return line.decode("utf-8", "replace").rstrip("\r\n")

    def _expect_ok(self, line=None):
        resp = self._readline() if line is None else line
        if not resp.startswith("OK"):
            raise LadpError(resp)
        return resp

    def cmd(self, line):
        self.sock.sendall((line + "\n").encode("utf-8"))
        return self._readline()

    # ---------- 命令 ----------
    def cmd_hello(self):
        return self._expect_ok(self.cmd("HELLO"))

    def ping(self):
        return self.cmd("PING")

    def mkdir(self, relpath):
        return self._expect_ok(self.cmd("MKDIR " + relpath))

    def rm(self, relpath):
        return self.cmd("RM " + relpath)

    def ls(self):
        """返回 [(id, meta行), ...]"""
        resp = self.cmd("LIST")
        if not resp.startswith("OK "):
            raise LadpError(resp)
        n = int(resp.split(" ", 1)[1])
        items = []
        for _ in range(n):
            line = self._readline()
            name, _, meta = line.partition("\t")
            items.append((name, meta))
        return items

    def put(self, relpath, data):
        """整块上传：先发头，再发原始字节，服务端按 CRC32 校验后原子落盘。"""
        crc = zlib.crc32(data) & 0xFFFFFFFF
        header = "PUT %d %08x %s\n" % (len(data), crc, relpath)
        self.sock.sendall(header.encode("utf-8") + data)
        return self._expect_ok()

    def close(self):
        try:
            self.cmd("BYE")
        except Exception:
            pass
        try:
            self.fp.close()
            self.sock.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# =====================================================================
# 媒体转码
# =====================================================================
def find_ffmpeg():
    cands = [
        os.environ.get("FFMPEG"),
        shutil.which("ffmpeg"),
        os.path.expanduser("~/tools/ffmpeg"),
        "/usr/bin/ffmpeg",
    ]
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    sys.exit("找不到 ffmpeg。请先安装，或设置环境变量 FFMPEG=/path/to/ffmpeg")


def run_ffmpeg(args):
    cmd = [find_ffmpeg(), "-hide_banner", "-loglevel", "error", "-y"] + args
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("ffmpeg 失败：\n%s" % (p.stderr.strip() or p.stdout.strip()))


def crop_filter(w, h):
    """等比放大到刚好盖住目标框，再居中裁切 —— 也就是 CSS 的 object-fit: cover。

    为什么要连续 crop 两次：
    第一步 increase 之后，长边会恰好等于目标长边（不裁），只有短边溢出。
    素材多是截图，四边常带一条杂色 / 边框，只在短边裁根本切不掉。
    所以先按 1.06 倍放大一个"大框"，再裁回目标尺寸 —— 相当于四周各切掉 3%，
    边缘脏东西就没了，顺带躲开 resize 在半像素处的振铃。"""
    w2 = int(round(w * 1.06))
    h2 = int(round(h * 1.06))
    return (
        "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d,crop=%d:%d"
        % (w2, h2, w2, h2, w, h)
    )


def sample_border_color(path):
    """取四角各一小块、缩成 1x1 求平均，得到素材的"环境底色"。

    用途：方形素材要放进 2.1:1 的横幅时，与其硬裁掉主体，不如按原样缩进去、
    四周用这个底色填满。白底商品图填出来就是纯白，完全看不出边界；
    彩色背景的照片填出来是一圈同色系衬底，也比生硬的模糊块自然。"""
    ffmpeg = find_ffmpeg()
    fc = (
        "[0:v]crop=iw*0.08:ih*0.08:0:0[a];"
        "[0:v]crop=iw*0.08:ih*0.08:iw*0.92:0[b];"
        "[0:v]crop=iw*0.08:ih*0.08:0:ih*0.92[c];"
        "[0:v]crop=iw*0.08:ih*0.08:iw*0.92:ih*0.92[d];"
        "[a][b][c][d]hstack=inputs=4,scale=1:1,format=rgb24"
    )
    p = subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", path,
         "-filter_complex", fc, "-frames:v", "1", "-f", "rawvideo", "-"],
        capture_output=True,
    )
    if p.returncode != 0 or len(p.stdout) < 3:
        return (0xFF, 0xFF, 0xFF)
    return tuple(p.stdout[:3])


def resolve_fit(src, w, h, fit="auto"):
    """把 fit="auto" 解析成具体策略。

    规则只有一条：**接近正方形（0.92~1.08）的素材不硬裁**。
    1:1 的素材硬裁成 2.1:1，等于把中间一条放大两三倍 —— 竖屏视频、方形海报
    到了板上就只剩半张脸。这种素材走 pad：等比缩进去，四周用环境底色填满，
    内容一点不切。
    """
    if fit != "auto":
        return fit

    sz = probe_size(src)
    if not sz:
        return "cover"

    ar = sz[0] / sz[1]
    return "pad" if 0.92 <= ar <= 1.08 else "cover"


def make_cover(src_image, out_jpg, w=CARD_W, h=CARD_H, quality=3, fit="auto"):
    """生成封面 / 缩略图。

    fit：
      cover  等比放大铺满再居中裁 —— 横图、竖图都适合，画面有张力
      pad    等比缩到装得下，四周用素材的环境底色填满 —— 方形商品图/手办图用这个，
             硬裁成 2.1:1 等于把中间一条放大两三倍，只剩半张脸
      blur   模糊底 + 完整主体居中 —— 想要点氛围感时用，但素材本身是纯色底时
             会出现一条明显的接缝，不如 pad
      auto   接近正方形（0.92~1.08）走 pad，其余走 cover
    """
    if fit == "auto":
        fit = resolve_fit(src_image, w, h, fit)

    if fit == "blur":
        fc = (
            "[0:v]scale={w}:{h}:force_original_aspect_ratio=increase,crop={w}:{h},"
            "gblur=sigma=18[bg];"
            "[0:v]scale={w}:{h}:force_original_aspect_ratio=decrease[fg];"
            "[bg][fg]overlay=(W-w)/2:(H-h)/2,format=yuvj420p"
        ).format(w=w, h=h)
        run_ffmpeg([
            "-i", src_image,
            "-filter_complex", fc,
            "-frames:v", "1",
            "-q:v", str(quality),
            out_jpg,
        ])
        return

    if fit == "pad":
        r, g, b = sample_border_color(src_image)
        fc = (
            "scale={w}:{h}:force_original_aspect_ratio=decrease,"
            "pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=0x{r:02x}{g:02x}{b:02x},format=yuvj420p"
        ).format(w=w, h=h, r=r, g=g, b=b)
        run_ffmpeg([
            "-i", src_image,
            "-vf", fc,
            "-frames:v", "1",
            "-q:v", str(quality),
            out_jpg,
        ])
        return

    run_ffmpeg([
        "-i", src_image,
        "-vf", crop_filter(w, h),
        "-frames:v", "1",
        "-pix_fmt", "yuvj420p",     # 必须！原因见文件头「JPEG 采样」注释
        "-q:v", str(quality),
        out_jpg,
    ])


def make_video_frames(src_video, out_dir, fps, fit="auto"):
    """抽帧。

    fit 必须和封面用同一个值：以前这里写死了 cover，结果出现
    "封面是完整的人、播起来只剩半张脸"这种自相矛盾的广告。
    """
    if fit == "auto":
        fit = resolve_fit(src_video, CARD_W, CARD_H, fit)

    if fit == "pad":
        r, g, b = sample_border_color(src_video)
        vf = ("scale=%d:%d:force_original_aspect_ratio=decrease,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x%02x%02x%02x"
              % (CARD_W, CARD_H, CARD_W, CARD_H, r, g, b))
    else:
        vf = crop_filter(CARD_W, CARD_H)

    run_ffmpeg([
        "-i", src_video,
        "-vf", "%s,fps=%d" % (vf, fps),
        "-pix_fmt", "yuvj420p",     # 必须！原因见文件头「JPEG 采样」注释
        "-q:v", "6",
        "-start_number", "0",
        os.path.join(out_dir, "f_%03d.jpg"),
    ])
    return sorted(f for f in os.listdir(out_dir) if f.startswith("f_") and f.endswith(".jpg"))


def _ffprobe_path():
    """ffprobe 一般和 ffmpeg 同目录；找不到就退回 PATH。"""
    cand = os.path.join(os.path.dirname(find_ffmpeg()), "ffprobe")
    if os.path.isfile(cand) and os.access(cand, os.X_OK):
        return cand
    return shutil.which("ffprobe")


def probe_size(path):
    """返回 (宽, 高)；拿不到就 None。"""
    ffprobe = _ffprobe_path()
    if not ffprobe:
        return None
    p = subprocess.run(
        [ffprobe, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
        capture_output=True, text=True,
    )
    try:
        w, h = p.stdout.strip().splitlines()[0].split(",")[:2]
        return int(w), int(h)
    except (ValueError, IndexError):
        return None


def probe_duration(src_video):
    """拿时长只是为了给个提示，拿不到就返回 0。"""
    ffprobe = _ffprobe_path()
    if not ffprobe:
        return 0.0
    p = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", src_video],
        capture_output=True, text=True,
    )
    try:
        return float(p.stdout.strip())
    except ValueError:
        return 0.0


# =====================================================================
# 打包
# =====================================================================
def build_package(workdir, ad_id, title, subtitle, video, cover, fps, hold,
                  fit="auto", remark=""):
    """把素材转成广告包目录，返回 (包目录, meta 行)。

    remark 是给客户填的备注（谁传的、什么档期、备注信息之类），
    存在清单的第 6 段，板上广告库里以小字显示在标题下面。"""
    for label, val in (("标题", title), ("副标题", subtitle), ("备注", remark)):
        if "|" in val:
            sys.exit("%s里不能有竖线 '|'（它是清单的分隔符）" % label)

    pkg = os.path.join(workdir, ad_id)
    os.makedirs(pkg, exist_ok=True)

    is_video = bool(video)

    # 先把 fit 定下来：以视频为准（没有视频就看封面图），
    # 之后封面和每一帧都用同一个值，不会再出现"封面完整、播放被切"。
    if video or cover:
        fit = resolve_fit(video or cover, CARD_W, CARD_H, fit)

    # 封面：优先用用户给的图片；只给视频时用视频首帧凑一张
    if cover:
        make_cover(cover, os.path.join(pkg, "cover.jpg"), fit=fit)
    elif video:
        make_cover(video, os.path.join(pkg, "cover.jpg"), fit=fit)
    else:
        sys.exit("图片广告必须用 --cover 指定一张图片")

    # 缩略图统一从 cover.jpg 再裁一次，保证和播放页同一构图
    make_cover(os.path.join(pkg, "cover.jpg"), os.path.join(pkg, "thumb.jpg"),
               THUMB_W, THUMB_H, quality=4)

    frames = []
    if is_video:
        dur = probe_duration(video)
        if dur > 0 and dur * fps > MAX_FRAMES:
            new_fps = max(4, int(MAX_FRAMES // dur))
            print("  ! 视频 %.1fs × %dfps 超过 %d 帧上限，自动降到 %dfps"
                  % (dur, fps, MAX_FRAMES, new_fps))
            fps = new_fps

        frames = make_video_frames(video, pkg, fps, fit=fit)
        if not frames:
            sys.exit("视频抽帧失败，检查一下文件是不是能正常播放")

    # 清单行：类型|标题|副标题|帧率|停留秒|备注
    kind = "V" if is_video else "I"
    meta = "%s|%s|%s|%d|%d|%s" % (kind, title, subtitle,
                                   fps if is_video else 0, hold, remark)
    with open(os.path.join(pkg, "meta.conf"), "w", encoding="utf-8") as f:
        f.write(meta + "\n")

    return pkg, meta, is_video, len(frames)


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f%s" % (n, unit) if unit != "B" else "%dB" % n
        n /= 1024.0


# =====================================================================
# 子命令
# =====================================================================
def cmd_pack(args):
    workdir = args.out or tempfile.mkdtemp(prefix="adpkg-")
    os.makedirs(workdir, exist_ok=True)

    pkg, meta, is_video, nframes = build_package(
        workdir, args.id, args.title, args.sub, args.video, args.cover,
        args.fps, args.hold, args.fit, remark=args.remark,
    )

    total = sum(
        os.path.getsize(os.path.join(dp, f))
        for dp, _, fs in os.walk(pkg) for f in fs
    )

    print("打包完成：%s" % pkg)
    print("  类型    : %s" % ("视频 / %d 帧" % nframes if is_video else "图片"))
    print("  清单行  : %s" % meta)
    print("  总大小  : %s" % human(total))
    print("\n上传到开发板：")
    print("  ./adclient.py push --id %s --from %s" % (args.id, pkg))
    return 0


def upload_package(cli, pkg, ad_id, progress=None):
    """先传媒体，最后才传 meta —— 板上的 UI 只认 meta，这样它永远不会看到半个广告。

    progress(i, total_files, sent_bytes, total_bytes) 是可选的进度回调，
    网页上传那条链路靠它把百分比推给浏览器。"""
    files = []
    for dp, _, fs in os.walk(pkg):
        for f in fs:
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, pkg).replace(os.sep, "/")
            files.append((rel, full))

    # meta.conf 排最后
    files.sort(key=lambda x: (x[0] == "meta.conf", x[0]))

    total_bytes = sum(os.path.getsize(p) for _, p in files)
    sent = 0

    for i, (rel, full) in enumerate(files, 1):
        with open(full, "rb") as f:
            data = f.read()

        if len(data) > MAX_FRAME_BYTES and rel.startswith("f_"):
            print("  ! %s 单帧 %.0fKB 偏大，解码可能跟不上" % (rel, len(data) / 1024))

        remote = "media/%s/%s" % (ad_id, rel) if rel != "meta.conf" else "meta/%s.conf" % ad_id
        cli.put(remote, data)

        sent += len(data)
        if progress is not None:
            progress(i, len(files), sent, total_bytes)
        if i % 10 == 0 or i == len(files):
            print("  [%3d/%d] %5.1f%%  %s" % (i, len(files), 100.0 * sent / total_bytes, rel))

    return len(files)


def cmd_push(args):
    if args.from_dir:
        pkg = args.from_dir
        if not os.path.isdir(pkg):
            sys.exit("目录不存在：%s" % pkg)
        ad_id = args.id or os.path.basename(os.path.normpath(pkg))
    else:
        if not args.title:
            sys.exit("push 需要 --title（或用 --from 直接推一个已打包的目录）")
        workdir = tempfile.mkdtemp(prefix="adpkg-")
        pkg, _, _, _ = build_package(
            workdir, args.id, args.title, args.sub, args.video, args.cover,
            args.fps, args.hold, args.fit, remark=args.remark,
        )
        ad_id = args.id

    print("连接 %s:%d ..." % (args.host, args.port))
    with LadpClient(args.host, args.port) as cli:
        print("  %s" % cli.banner)
        n = upload_package(cli, pkg, ad_id)
        print("上传完成，共 %d 个文件" % n)
    return 0


def cmd_push_tree(args):
    """把一个"板上布局"的目录整棵树推上去：
         <dir>/media/<id>/...  +  <dir>/meta/<id>.conf
       每条广告都是"先媒体、后 meta"，所以板上永远不会出现半条广告。"""
    root = args.dir
    media_dir = os.path.join(root, "media")
    meta_dir = os.path.join(root, "meta")

    if not os.path.isdir(media_dir):
        sys.exit("目录里没有 media/：%s" % root)

    ad_ids = sorted(d for d in os.listdir(media_dir)
                    if os.path.isdir(os.path.join(media_dir, d)))
    if not ad_ids:
        sys.exit("media/ 下没有广告目录")

    print("连接 %s:%d ..." % (args.host, args.port))
    with LadpClient(args.host, args.port) as cli:
        print("  %s" % cli.banner)

        for ad_id in ad_ids:
            ad_dir = os.path.join(media_dir, ad_id)
            files = sorted(
                os.path.relpath(os.path.join(dp, f), ad_dir).replace(os.sep, "/")
                for dp, _, fs in os.walk(ad_dir) for f in fs
            )
            for rel in files:
                with open(os.path.join(ad_dir, rel), "rb") as f:
                    cli.put("media/%s/%s" % (ad_id, rel), f.read())

            conf = os.path.join(meta_dir, ad_id + ".conf")
            if os.path.isfile(conf):
                with open(conf, "rb") as f:
                    cli.put("meta/%s.conf" % ad_id, f.read())
                print("  %-12s %3d 个文件  ok" % (ad_id, len(files)))
            else:
                print("  %-12s %3d 个文件  （缺 meta/%s.conf，跳过清单）"
                      % (ad_id, len(files), ad_id))
    return 0


def cmd_list(args):
    with LadpClient(args.host, args.port) as cli:
        print("  %s\n" % cli.banner)
        items = cli.ls()
        if not items:
            print("板上还没有广告。用 push 传一个吧。")
            return 0

        print("%-4s %-20s %-4s %-8s %s" % ("#", "ID", "类型", "帧率", "标题"))
        print("-" * 72)
        for i, (name, meta) in enumerate(sorted(items), 1):
            f = meta.split("|")
            f += [""] * (5 - len(f))
            kind = "视频" if f[0].upper() == "V" else "图片"
            print("%-4d %-20s %-4s %-8s %s" % (i, name, kind, f[3] or "-", f[1]))
    return 0


def cmd_rm(args):
    with LadpClient(args.host, args.port) as cli:
        # 先删 meta：UI 立刻看不到这条；再删媒体，避免删到一半索引还在
        print(cli.rm("meta/%s.conf" % args.id))
        print(cli.rm("media/%s" % args.id))
    print("已删除 %s" % args.id)
    return 0


def cmd_ping(args):
    with LadpClient(args.host, args.port) as cli:
        print(cli.banner)
        print(cli.ping())
    return 0


def cmd_ticker(args):
    """改屏幕最底下那条滚动公告。文案存在 <AD_ROOT>/ticker.conf，只取第一行。"""
    if args.reset:
        with LadpClient(args.host, args.port) as cli:
            print(cli.rm("ticker.conf"))
        print("已删除 ticker.conf，板上恢复内置默认文案")
        return 0

    text = (args.text or "").strip()
    if not text:
        sys.exit("--text 不能为空（要恢复默认用 --reset）")

    data = (text + "\n").encode("utf-8")
    with LadpClient(args.host, args.port) as cli:
        print(cli.put("ticker.conf", data))
    print("底部公告已更新，板上约 1 秒内滚动起来")
    return 0


# =====================================================================
def main():
    ap = argparse.ArgumentParser(
        description="云屏广告机 · 电脑端上传客户端",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--host", default="172.20.120.220", help="开发板 IP（默认 172.20.120.220）")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="端口（默认 9500）")

    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_pack_args(p):
        p.add_argument("--id", required=True, help="广告 ID，也是板上目录名，建议 01_xxx")
        p.add_argument("--title", help="中文标题")
        p.add_argument("--sub", default="", help="英文副标题")
        p.add_argument("--video", help="视频文件（不给就是纯图片广告）")
        p.add_argument("--cover", help="封面图片")
        p.add_argument("--fps", type=int, default=12, help="抽帧帧率（默认 12）")
        p.add_argument("--hold", type=int, default=5, help="图片广告停留秒数（默认 5）")
        p.add_argument("--fit", choices=("auto", "cover", "pad", "blur"), default="auto",
                       help="封面裁切方式：auto 自动 / cover 铺满裁切 / pad 纯色底居中 / blur 模糊底居中")
        p.add_argument("--remark", default="", help="备注，显示在广告库标题下方")

    p_pack = sub.add_parser("pack", help="只打包到本地，不上传")
    add_pack_args(p_pack)
    p_pack.add_argument("--out", help="输出目录（默认临时目录）")
    p_pack.set_defaults(func=cmd_pack)

    p_push = sub.add_parser("push", help="打包并上传")
    add_pack_args(p_push)
    p_push.add_argument("--from", dest="from_dir", help="直接推一个已打包好的目录")
    p_push.set_defaults(func=cmd_push)

    p_list = sub.add_parser("list", help="列出板上的广告")
    p_list.set_defaults(func=cmd_list)

    p_tree = sub.add_parser("push-tree", help="上传一个整棵「板上布局」的目录")
    p_tree.add_argument("--dir", required=True, help="含 media/ 与 meta/ 的目录")
    p_tree.set_defaults(func=cmd_push_tree)

    p_rm = sub.add_parser("rm", help="删除一个广告")
    p_rm.add_argument("--id", required=True)
    p_rm.set_defaults(func=cmd_rm)

    p_ping = sub.add_parser("ping", help="测试连通性")
    p_ping.set_defaults(func=cmd_ping)

    p_ticker = sub.add_parser("ticker", help="改屏幕底部那条滚动公告")
    p_ticker.add_argument("--text", help="公告文案（UTF-8）")
    p_ticker.add_argument("--reset", action="store_true", help="删掉自定义文案，恢复内置默认")
    p_ticker.set_defaults(func=cmd_ticker)

    args = ap.parse_args()
    try:
        return args.func(args)
    except (LadpError, RuntimeError, OSError) as e:
        sys.exit("失败：%s" % e)


if __name__ == "__main__":
    sys.exit(main())
