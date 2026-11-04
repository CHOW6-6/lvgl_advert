#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
weather_dashboard.py —— 天气刷新看板（网页版）

把"天气到底刷没刷新、刷成什么、推没推上去"摊在一个页面上，省得每次
开终端 cat 日志。数据全从本地文件读，不额外依赖任何东西。

页面内容：
  · 当前缓存里的天气（城市/温度/天气/湿度/风/更新时间/数据源）
  · 定时任务状态（crontab 里那条）
  · 实时日志滚动（weather_watch.py 的输出，每 2 秒拉一次增量）
  · 最近一次推送结果

用法：
    python3 weather/tools/weather_dashboard.py                # http://0.0.0.0:8090
    python3 weather/tools/weather_dashboard.py --port 9000
    python3 weather/tools/weather_dashboard.py --log /tmp/weather_watch.log

然后浏览器打开 http://localhost:8090
"""

import argparse
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))

STATE = {
    "cache": os.path.join(PROJ, "weather_cache.txt"),
    "log": "/tmp/weather_watch.log",
    "conf": os.path.join(PROJ, "weather", "config", "weather_conf.json"),
}

LOCK = threading.Lock()


def read_text(path, limit=None):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            data = f.read()
        return data[-limit:] if limit else data
    except OSError:
        return ""


def parse_cache():
    """weather_cache.txt 是 key=value，直接翻成字典"""
    out = {}
    for line in read_text(STATE["cache"]).splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k] = v
    return out


def cron_line():
    try:
        r = subprocess.run(["crontab", "-l"], capture_output=True, text=True, timeout=5)
        for line in r.stdout.splitlines():
            if "lvgl-weather-watch" in line:
                return line.strip()
    except Exception:
        pass
    return ""


def board_reachable():
    """板子 SSH 端口开不开（1 秒超时，别把页面拖住）"""
    import socket
    try:
        with socket.create_connection(("172.20.120.220", 22), timeout=1):
            return True
    except OSError:
        return False


def relay_alive():
    """Windows 上的 MQTT 中转在不在（板子连的是它）"""
    import socket
    try:
        with socket.create_connection(("172.20.120.100", 1883), timeout=1):
            return True
    except OSError:
        return False


def log_tail(max_bytes=64 * 1024):
    return read_text(STATE["log"], limit=max_bytes)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):        # 别把请求日志刷到终端
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(data)
        except BrokenPipeError:
            pass

    def do_GET(self):
        if self.path.startswith("/api/state"):
            with LOCK:
                payload = {
                    "now": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "cache": parse_cache(),
                    "cron": cron_line(),
                    "board": board_reachable(),
                    "relay": relay_alive(),
                }
            self._send(200, json.dumps(payload, ensure_ascii=False))
            return

        if self.path.startswith("/api/log"):
            # 只回增量：?offset=字节数
            offset = 0
            if "offset=" in self.path:
                try:
                    offset = int(self.path.split("offset=")[1].split("&")[0])
                except ValueError:
                    offset = 0
            text = log_tail()
            if offset > len(text):            # 日志轮转过，从头来
                offset = 0
            self._send(200, json.dumps({
                "offset": len(text),
                "text": text[offset:],
            }, ensure_ascii=False))
            return

        if self.path in ("/", "/index.html"):
            self._send(200, INDEX_HTML, "text/html; charset=utf-8")
            return

        self._send(404, json.dumps({"error": "not found"}))


INDEX_HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>天气刷新看板</title>
<style>
  :root{
    --bg:#0f1622; --card:#1b2534; --card2:#16202e; --ink:#e8eef7;
    --ink2:#9fb3c8; --ok:#2ec19d; --warn:#ffb04a; --bad:#ef6b6b; --line:#25344a;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);
       font:14px/1.6 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
  .wrap{max-width:1040px;margin:0 auto;padding:22px 18px 40px}
  h1{font-size:18px;margin:0 0 4px;font-weight:600;letter-spacing:.5px}
  .sub{color:var(--ink2);font-size:12px;margin-bottom:18px}
  .grid{display:grid;grid-template-columns:1.2fr 1fr;gap:14px}
  @media(max-width:760px){.grid{grid-template-columns:1fr}}
  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px 18px}
  .card h2{font-size:13px;color:var(--ink2);font-weight:500;margin:0 0 12px;
           letter-spacing:.4px;text-transform:none}
  .big{font-size:44px;font-weight:600;line-height:1.1}
  .city{font-size:17px;color:#bfd7ff}
  .row{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px dashed #1f2c3d}
  .row:last-child{border-bottom:0}
  .k{color:var(--ink2)}
  .v{font-variant-numeric:tabular-nums}
  .ok{color:var(--ok)} .warn{color:var(--warn)} .bad{color:var(--bad)}
  .pill{display:inline-block;padding:1px 9px;border-radius:999px;font-size:12px;
        border:1px solid currentColor}
  #log{background:#0b111a;border:1px solid var(--line);border-radius:12px;
       padding:12px 14px;height:340px;overflow:auto;font:12px/1.75
       ui-monospace,Menlo,Consolas,"Courier New",monospace;white-space:pre-wrap}
  #log .t{color:#6f8aa6}
  #log .push{color:var(--ok)}
  #log .fail{color:var(--bad)}
  #log .skip{color:#6f8aa6}
  .note{color:var(--ink2);font-size:12px;margin-top:8px}
  button{background:#22314a;color:var(--ink);border:1px solid var(--line);
         border-radius:9px;padding:5px 12px;font-size:12px;cursor:pointer}
  button:hover{background:#2a3c58}
</style>
</head>
<body>
<div class="wrap">
  <h1>天气刷新看板</h1>
  <div class="sub">页面每 2 秒自动拉一次 · <span id="clock">--</span></div>

  <div class="grid">
    <div class="card">
      <h2>当前天气（板子上显示的就是这份缓存）</h2>
      <div class="city" id="city">--</div>
      <div class="big" id="temp">--°C</div>
      <div class="note" id="text">--</div>
      <div style="margin-top:12px">
        <div class="row"><span class="k">湿度</span><span class="v" id="hum">--</span></div>
        <div class="row"><span class="k">风速</span><span class="v" id="wind">--</span></div>
        <div class="row"><span class="k">数据更新时间</span><span class="v" id="updated">--</span></div>
        <div class="row"><span class="k">数据源</span><span class="v" id="src">--</span></div>
      </div>
    </div>

    <div class="card">
      <h2>运行状态</h2>
      <div class="row"><span class="k">定时刷新</span><span class="v" id="cron">--</span></div>
      <div class="row"><span class="k">板子 SSH</span><span class="v" id="board">--</span></div>
      <div class="row"><span class="k">MQTT 中转</span><span class="v" id="relay">--</span></div>
      <div class="row"><span class="k">最近一次推送</span><span class="v" id="lastpush">--</span></div>
      <div style="margin-top:14px;display:flex;gap:8px">
        <button onclick="togglePause()" id="btn">暂停滚动</button>
        <button onclick="clearView()">清屏（不动日志文件）</button>
      </div>
      <div class="note">
        定时任务没装？<code>make weather-cron-install</code>；
        中转没开？双击 <code>weather/tools/relay-win.bat</code>。
      </div>
    </div>
  </div>

  <div class="card" style="margin-top:14px">
    <h2>天气刷新日志（/tmp/weather_watch.log 实时增量）</h2>
    <div id="log"></div>
  </div>
</div>

<script>
let offset = 0, paused = false, lastPush = "";

function cls(line){
  if(line.includes("已推送")) return "push";
  if(line.includes("失败"))   return "fail";
  if(line.includes("跳过"))   return "skip";
  return "";
}
function appendLog(text){
  if(!text) return;
  const box = document.getElementById("log");
  const stick = box.scrollTop + box.clientHeight >= box.scrollHeight - 40;
  for(const line of text.split("\n")){
    if(!line.trim()) continue;
    const div = document.createElement("div");
    div.className = cls(line);
    div.textContent = line;
    box.appendChild(div);
    if(line.includes("已推送") || line.includes("推送失败")) lastPush = line;
  }
  if(stick && !paused) box.scrollTop = box.scrollHeight;
}
function togglePause(){
  paused = !paused;
  document.getElementById("btn").textContent = paused ? "继续滚动" : "暂停滚动";
}
function clearView(){
  document.getElementById("log").innerHTML = "";
}

async function tick(){
  try{
    const s = await (await fetch("/api/state")).json();
    document.getElementById("clock").textContent = s.now;
    const c = s.cache || {};
    document.getElementById("city").textContent = c.city || "--";
    document.getElementById("temp").textContent = (c.temp ?? "--") + "°C";
    document.getElementById("text").textContent = c.text || "--";
    document.getElementById("hum").textContent = (c.humidity ?? "--") + " %";
    document.getElementById("wind").textContent = (c.wind ?? "--") + " km/h";
    document.getElementById("updated").textContent = c.updated || "--";
    document.getElementById("src").textContent = c.source || "缓存";

    const cronEl = document.getElementById("cron");
    if(s.cron){ cronEl.innerHTML = '<span class="ok">已装</span>'; cronEl.title = s.cron; }
    else { cronEl.innerHTML = '<span class="warn">未装</span>'; }

    document.getElementById("board").innerHTML = s.board
      ? '<span class="ok">在线</span>' : '<span class="bad">连不上</span>';
    document.getElementById("relay").innerHTML = s.relay
      ? '<span class="ok">在跑</span>' : '<span class="bad">没开</span>';

    const lp = document.getElementById("lastpush");
    lp.textContent = lastPush ? lastPush.replace(/^\[[^\]]+\]\s*/, "") : "--";
    lp.className = "v " + (lastPush.includes("失败") ? "bad" : "ok");

    const l = await (await fetch("/api/log?offset=" + offset)).json();
    offset = l.offset;
    appendLog(l.text);
  }catch(e){ /* 服务器重启时忽略 */ }
  setTimeout(tick, 2000);
}
tick();
</script>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(description="天气刷新看板")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--log", default=STATE["log"])
    ap.add_argument("--cache", default=STATE["cache"])
    args = ap.parse_args()

    STATE["log"] = args.log
    STATE["cache"] = args.cache

    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    srv.daemon_threads = True
    print("[dashboard] http://localhost:%d  (绑定 %s)" % (args.port, args.bind), flush=True)
    print("[dashboard] 日志: %s" % args.log, flush=True)
    print("[dashboard] 缓存: %s" % args.cache, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[dashboard] 退出", flush=True)
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
