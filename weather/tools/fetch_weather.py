#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_weather.py —— 在电脑（WSL/主机）上抓心知天气，写成板子能读的缓存文件。

为什么要绕这一圈：
  GEC6818 的默认网关是坏的（172.20.127.255），板子既没有可用路由也没有 DNS，
  在上面直接 HTTPS 调心知必然超时。而电脑有网、Python 自带 ssl，
  抓完用 ssh 管道丢过去，板子不联网也能显示真实天气。

用法：
  python3 weather/tools/fetch_weather.py                       # 只写 weather_cache.txt
  python3 weather/tools/fetch_weather.py --push                # 写完顺手推到板子
  python3 weather/tools/fetch_weather.py --push --board root@172.20.120.220
  python3 weather/tools/fetch_weather.py --location 广州 --days 3

密钥/城市默认从 weather/config/weather_conf.json 读（seniverseKey / location），
也可以用 --key / --location 覆盖，或用环境变量 WEATHER_SENIVERSE_KEY / WEATHER_LOCATION。
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))          # .../lv_port_pc_vscode-release-v8
DEFAULT_CONF = os.path.join(PROJ, "weather", "config", "weather_conf.json")
API = "https://api.seniverse.com/v3/weather/{}.json"


def load_conf(path):
    """读配置；读不到就返回空 dict（后面还有命令行/环境变量兜底）。"""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        print("[warn] 读不到配置 %s：%s" % (path, e), file=sys.stderr)
        return {}


def api_get(endpoint, key, location, retries=3, **extra):
    params = {"key": key, "location": location, "language": "zh-Hans", "unit": "c"}
    params.update(extra)
    url = API.format(endpoint) + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": "lvgl-weather/1.0"})

    # 为什么要重试：WSL 的 DNS 代理（/etc/resolv.conf 里的 10.255.255.254）偶尔会
    # 漏答一次，Python 就抛 "[Errno -3] Temporary failure in name resolution"。
    # 这是瞬时故障（不是断网），重试一两次基本就过去了；以前一次抖动就让
    # make weather-data 整个失败、旧缓存不动，日志里全是这一条。
    for i in range(max(1, retries)):
        try:
            with urllib.request.urlopen(req, timeout=12) as r:
                data = json.loads(r.read().decode("utf-8"))
            break
        except urllib.error.URLError as e:
            if i + 1 >= retries:          # 最后一次了，原样抛出去（保留 traceback）
                raise
            wait = 1.5 * (i + 1)
            hint = ""
            if getattr(getattr(e, "reason", None), "errno", None) in (-2, -3):
                hint = "（域名解析失败，通常是 WSL 的 DNS 代理瞬时无响应，不是断网）"
            print("[warn] 第 %d/%d 次请求失败%s：%s —— %.1fs 后重试"
                  % (i + 1, retries, hint, e, wait), file=sys.stderr)
            time.sleep(wait)
    if "results" not in data or not data["results"]:
        raise RuntimeError("心知返回异常：%s" % json.dumps(data, ensure_ascii=False)[:200])
    return data["results"][0]


def fmt_time(iso):
    """2026-11-02T15:04:08+08:00 -> 11-02 15:04（和板端显示格式一致）"""
    if not isinstance(iso, str) or len(iso) < 16:
        return "--"
    return iso[5:10] + " " + iso[11:16]


def build_cache(now_r, daily_r, days):
    now = now_r.get("now", {})
    loc = now_r.get("location", {})
    daily = (daily_r or {}).get("daily", [])[:days]

    # now 接口只给 text/code/temperature；湿度和风速得从 daily[0] 借
    d0 = daily[0] if daily else {}

    lines = [
        "# 由 weather/tools/fetch_weather.py 自动生成，不要手改",
        "ts=%d" % int(time.time()),
        "city=%s" % loc.get("name", "?"),
        "text=%s" % now.get("text", "--"),
        "code=%s" % now.get("code", "0"),
        "temp=%s" % now.get("temperature", "0"),
        "humidity=%s" % d0.get("humidity", "0"),
        "wind=%s" % d0.get("wind_speed", "0"),
        "updated=%s" % fmt_time(now_r.get("last_update")),
        "days=%d" % len(daily),
    ]
    for i, d in enumerate(daily):
        lines.append("d%d_date=%s" % (i, d.get("date", "")))
        lines.append("d%d_text=%s" % (i, d.get("text_day", "--")))
        lines.append("d%d_high=%s" % (i, d.get("high", "0")))
        lines.append("d%d_low=%s" % (i, d.get("low", "0")))

    # 缓存里全是 UTF-8 中文，板端按字节读，别加 BOM
    return "\n".join(lines) + "\n"


def push(content, board, dest):
    """走 ssh 管道写文件（板上 dropbear 没有 sftp 子系统，scp 会失败）。"""
    cmd = ["ssh", "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=8",
           board, "cat > %s.tmp && mv %s.tmp %s" % (dest, dest, dest)]
    p = subprocess.run(cmd, input=content.encode("utf-8"), capture_output=True)
    if p.returncode != 0:
        raise RuntimeError("推送失败：%s" % p.stderr.decode("utf-8", "ignore").strip())
    print("[ok] 已推送到 %s:%s" % (board, dest))


def main():
    ap = argparse.ArgumentParser(description="抓心知天气并生成板端缓存")
    ap.add_argument("--conf", default=DEFAULT_CONF, help="配置文件路径")
    ap.add_argument("--key", default=os.environ.get("WEATHER_SENIVERSE_KEY"), help="心知私钥")
    ap.add_argument("--location", default=os.environ.get("WEATHER_LOCATION"), help="城市，如 深圳/beijing")
    ap.add_argument("--days", type=int, default=3, help="预报天数（最多 3，和板端数组一致）")
    ap.add_argument("--out", default="weather_cache.txt", help="输出的缓存文件")
    ap.add_argument("--push", action="store_true", help="写完推到板子")
    ap.add_argument("--board", default="root@172.20.120.220")
    ap.add_argument("--dest", default="/root/lvgl-weather/weather_cache.txt")
    args = ap.parse_args()

    conf = load_conf(args.conf)
    key = args.key or conf.get("seniverseKey") or ""
    location = args.location or conf.get("location") or "深圳"
    days = max(1, min(args.days, 3))

    if not key:
        print("[err] 没有心知私钥：在 %s 里填 seniverseKey，或用 --key 指定" % args.conf,
              file=sys.stderr)
        return 2

    try:
        now_r = api_get("now", key, location)
        try:
            daily_r = api_get("daily", key, location, start=0, days=days)
        except Exception as e:                      # 预报失败不影响实况
            print("[warn] 预报接口失败，只写实况：%s" % e, file=sys.stderr)
            daily_r = {"daily": []}
    except Exception as e:
        # 抓失败就保留旧缓存 —— 板上留着上一次的真数据，总比刷成随机数强
        print("[err] 抓取失败：%s" % e, file=sys.stderr)
        print("[err] 旧缓存保持不变", file=sys.stderr)
        return 1

    content = build_cache(now_r, daily_r, days)

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("[ok] 已写入 %s" % args.out)
    for line in content.splitlines():
        if line and not line.startswith("#"):
            print("     " + line)

    if args.push:
        try:
            push(content, args.board, args.dest)
        except Exception as e:
            print("[err] %s" % e, file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
