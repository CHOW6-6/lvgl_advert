#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
webupload.py —— 广告机素材上传服务（网页版）

给客户用的界面，不用碰命令行：浏览器里选视频、选封面图、填标题和备注，
点一下就把广告送到开发板上。转码、打包、CRC 校验、推流全在后台做。

跑在哪：跑在**电脑（WSL/Windows）**上，不是开发板上。
        开发板那一侧完全不变，还是原来的 ladpd 接收 + demo 播放。

为什么用标准库：
     客户机器上不一定有 Flask/FastAPI，`python3 webupload.py` 就能跑，
     最省事的才是最好用的。multipart 解析自己写，一共四十行。

用法：
    python3 advert/web/webupload.py                 # 默认 0.0.0.0:8080
    python3 advert/web/webupload.py --port 9000
    python3 advert/web/webupload.py --board 172.20.120.220 --board-port 9500

然后在浏览器打开 http://localhost:8080
"""

import argparse
import html
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import traceback
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tcp"))
import adclient  # noqa: E402  复用现有的转码/打包/上传逻辑，不重复造一遍

BOARD_HOST = "172.20.120.220"
BOARD_PORT = 9500

MAX_UPLOAD = 300 * 1024 * 1024      # 单次请求上限 300MB

# ---------------------------------------------------------------- 任务表

JOBS = {}
JOBS_LOCK = threading.Lock()


def job_new():
    jid = uuid.uuid4().hex[:12]
    with JOBS_LOCK:
        JOBS[jid] = {
            "stage": "排队中", "percent": 0, "detail": "",
            "done": False, "error": None, "ad_id": None, "started": time.time(),
        }
    return jid


def job_set(jid, **kw):
    with JOBS_LOCK:
        if jid in JOBS:
            JOBS[jid].update(kw)


def job_get(jid):
    with JOBS_LOCK:
        j = JOBS.get(jid)
        return dict(j) if j else None


# ---------------------------------------------------------------- multipart

def parse_multipart(body, boundary):
    """极简 multipart/form-data 解析。

    返回 (普通字段 dict, 文件 dict)；文件值是 (文件名, 原始字节)。
    python 3.13 已经把 cgi 模块删了，所以自己拆 —— 反正格式就这么点东西。
    """
    fields, files = {}, {}
    delim = b"--" + boundary

    for part in body.split(delim)[1:]:
        if part.startswith(b"--"):        # 结束标记
            break

        part = part.lstrip(b"\r\n")
        head_end = part.find(b"\r\n\r\n")
        if head_end < 0:
            continue

        head = part[:head_end].decode("utf-8", "replace")
        data = part[head_end + 4:]
        if data.endswith(b"\r\n"):
            data = data[:-2]

        name = filename = None
        for line in head.split("\r\n"):
            if not line.lower().startswith("content-disposition:"):
                continue
            for seg in line.split(";"):
                seg = seg.strip()
                if seg.startswith("name="):
                    name = seg[5:].strip('"')
                elif seg.startswith("filename="):
                    filename = seg[9:].strip('"')

        if not name:
            continue
        if filename:
            files[name] = (filename, data)
        else:
            fields[name] = data.decode("utf-8", "replace")

    return fields, files


# ---------------------------------------------------------------- 后台任务

def run_job(jid, ad_id, title, sub, remark, video_path, cover_path, fps, hold, fit):
    workdir = None
    try:
        job_set(jid, stage="转码中", percent=4,
                detail="正在抽帧（长视频要几十秒，别关页面）")

        workdir = tempfile.mkdtemp(prefix="adweb-")
        pkg, meta, is_video, nframes = adclient.build_package(
            workdir, ad_id, title, sub, video_path, cover_path,
            fps, hold, fit=fit, remark=remark,
        )

        job_set(jid, stage="上传中", percent=25,
                detail="%d 帧已转好，开始推送" % nframes)

        def on_progress(i, total, sent, total_bytes):
            pct = 25 + int(72.0 * sent / max(1, total_bytes))
            job_set(jid, percent=pct, detail="%d/%d 个文件" % (i, total))

        with adclient.LadpClient(BOARD_HOST, BOARD_PORT) as cli:
            adclient.upload_package(cli, pkg, ad_id, progress=on_progress)

        job_set(jid, stage="完成", percent=100, done=True,
                detail="广告已上板，1 秒内自动出现在屏幕上")

    except (Exception, SystemExit) as e:      # build_package 出错会 sys.exit
        job_set(jid, stage="失败", done=True, error=str(e) or "未知错误")
        traceback.print_exc()
    finally:
        if workdir:
            shutil.rmtree(workdir, ignore_errors=True)
        for p in (video_path, cover_path):
            if p and os.path.isfile(p):
                try:
                    os.remove(p)
                except OSError:
                    pass


# ---------------------------------------------------------------- HTTP

INDEX_PATH = os.path.join(HERE, "index.html")


class Handler(BaseHTTPRequestHandler):
    server_version = "AdUpload/1.0"

    # ---------- 小工具 ----------
    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")

        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("[web] %s - %s\n" % (self.address_string(), fmt % args))

    # ---------- GET ----------
    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)

        if u.path in ("/", "/index.html"):
            try:
                with open(INDEX_PATH, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            except OSError:
                self._send(500, "找不到 index.html")
            return

        if u.path == "/api/ping":
            try:
                with adclient.LadpClient(BOARD_HOST, BOARD_PORT, timeout=5) as cli:
                    self._send(200, {"ok": True, "banner": cli.banner,
                                     "board": "%s:%d" % (BOARD_HOST, BOARD_PORT)})
            except Exception as e:
                self._send(200, {"ok": False, "error": str(e),
                                 "board": "%s:%d" % (BOARD_HOST, BOARD_PORT)})
            return

        if u.path == "/api/list":
            try:
                with adclient.LadpClient(BOARD_HOST, BOARD_PORT, timeout=8) as cli:
                    items = []
                    for name, meta in sorted(cli.ls()):
                        f = meta.split("|")
                        f += [""] * (6 - len(f))
                        items.append({
                            "id": name,
                            "type": "视频" if f[0].upper() == "V" else "图片",
                            "title": f[1],
                            "subtitle": f[2],
                            "fps": f[3],
                            "remark": f[5],
                        })
                self._send(200, {"ok": True, "items": items})
            except Exception as e:
                self._send(200, {"ok": False, "error": str(e), "items": []})
            return

        if u.path == "/api/status":
            jid = (q.get("id") or [""])[0]
            j = job_get(jid)
            if j is None:
                self._send(404, {"ok": False, "error": "任务不存在"})
            else:
                self._send(200, {"ok": True, "job": j})
            return

        self._send(404, {"ok": False, "error": "not found"})

    # ---------- POST ----------
    def do_POST(self):
        u = urlparse(self.path)

        if u.path == "/api/delete":
            n = int(self.headers.get("Content-Length") or 0)
            form = parse_qs(self.rfile.read(n).decode("utf-8", "replace"))
            ad_id = (form.get("id") or [""])[0]
            if not ad_id or "/" in ad_id or ".." in ad_id:
                self._send(400, {"ok": False, "error": "非法 ID"})
                return
            try:
                with adclient.LadpClient(BOARD_HOST, BOARD_PORT) as cli:
                    cli.rm("meta/%s.conf" % ad_id)
                    cli.rm("media/%s" % ad_id)
                self._send(200, {"ok": True})
            except Exception as e:
                self._send(200, {"ok": False, "error": str(e)})
            return

        if u.path != "/api/upload":
            self._send(404, {"ok": False, "error": "not found"})
            return

        ctype = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in ctype:
            self._send(400, {"ok": False, "error": "需要 multipart/form-data"})
            return

        try:
            boundary = ctype.split("boundary=", 1)[1].strip().strip('"').encode()
        except IndexError:
            self._send(400, {"ok": False, "error": "缺少 boundary"})
            return

        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_UPLOAD:
            self._send(413, {"ok": False,
                             "error": "文件太大（上限 %d MB）" % (MAX_UPLOAD // 1024 // 1024)})
            return

        body = self.rfile.read(length)
        fields, files = parse_multipart(body, boundary)

        title = (fields.get("title") or "").strip()
        sub = (fields.get("subtitle") or "").strip()
        remark = (fields.get("remark") or "").strip()
        try:
            fps = max(4, min(20, int(fields.get("fps") or 12)))
            hold = max(2, min(60, int(fields.get("hold") or 5)))
        except ValueError:
            fps, hold = 12, 5
        fit = fields.get("fit") or "auto"

        if not title:
            self._send(400, {"ok": False, "error": "标题不能为空"})
            return
        if "video" not in files and "cover" not in files:
            self._send(400, {"ok": False, "error": "至少要有视频或封面图"})
            return

        # 落盘：文件名里的中文/空格在这里被丢掉，只保留扩展名
        tmpdir = tempfile.mkdtemp(prefix="adupl-")
        video_path = cover_path = None

        try:
            if "video" in files:
                name, data = files["video"]
                ext = os.path.splitext(name)[1] or ".mp4"
                video_path = os.path.join(tmpdir, "video" + ext)
                with open(video_path, "wb") as f:
                    f.write(data)

            if "cover" in files:
                name, data = files["cover"]
                ext = os.path.splitext(name)[1] or ".jpg"
                cover_path = os.path.join(tmpdir, "cover" + ext)
                with open(cover_path, "wb") as f:
                    f.write(data)
        except OSError as e:
            self._send(500, {"ok": False, "error": "保存上传文件失败：%s" % e})
            return

        ad_id = time.strftime("ad_%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:4]

        jid = job_new()
        job_set(jid, ad_id=ad_id)
        threading.Thread(
            target=run_job,
            args=(jid, ad_id, title, sub, remark, video_path, cover_path, fps, hold, fit),
            daemon=True,
        ).start()

        self._send(200, {"ok": True, "job": jid, "id": ad_id})


def main():
    global BOARD_HOST, BOARD_PORT

    ap = argparse.ArgumentParser(description="广告机素材上传服务（网页版）")
    ap.add_argument("--host", default="0.0.0.0", help="监听地址（默认 0.0.0.0）")
    ap.add_argument("--port", type=int, default=8080, help="监听端口（默认 8080）")
    ap.add_argument("--board", default=BOARD_HOST, help="开发板 IP")
    ap.add_argument("--board-port", type=int, default=BOARD_PORT, help="开发板 LADP 端口")
    args = ap.parse_args()

    BOARD_HOST = args.board
    BOARD_PORT = args.board_port

    if not os.path.isfile(INDEX_PATH):
        sys.exit("找不到页面文件：%s" % INDEX_PATH)

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True

    print("=" * 56)
    print(" 广告机素材上传服务")
    print("   本机访问 : http://localhost:%d" % args.port)
    print("   开发板   : %s:%d" % (BOARD_HOST, BOARD_PORT))
    print("   Ctrl+C 结束")
    print("=" * 56)

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
