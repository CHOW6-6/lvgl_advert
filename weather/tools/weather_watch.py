#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
weather_watch.py —— 定时刷新天气：主机抓心知 -> 变了才推给板子。

板子不联网，天气只能由电脑抓了推过去（见 mqtts_relay.py 顶部说明）。
本脚本负责"一直抓、一直推"，不用你再手动 make weather-data。

用法：
    python3 weather/tools/weather_watch.py                  # 常驻，默认 5 分钟一轮
    python3 weather/tools/weather_watch.py --interval 60    # 1 分钟一轮
    python3 weather/tools/weather_watch.py --once           # 只跑一轮（计划任务用）
    python3 weather/tools/weather_watch.py --once --no-push # 只更新本机缓存

配合 Windows 计划任务（推荐，关掉终端也照跑）：
    make weather-task-install      # 装一个每 5 分钟跑一次的计划任务
    make weather-task-remove       # 卸载

判断"要不要推"：比对新旧缓存里除 ts 之外的内容。心知的 last_update 大约
十分钟才动一次，所以 5 分钟一轮里有一半是"没变化、不推"，省得每轮都 ssh。
"""

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import fetch_weather as fw  # noqa: E402  同目录，复用抓取/推送逻辑


def fingerprint(text):
    """去掉 ts= 那行后的内容指纹：允许 ts 变化而判定为"没更新"。"""
    return "\n".join(l for l in text.splitlines() if not l.startswith("ts="))


def log(msg):
    print(time.strftime("[%m-%d %H:%M:%S] ") + msg, flush=True)


def run_once(args):
    """抓一轮。返回 True=推了/写了新数据，False=没变化。"""
    conf = fw.load_conf(args.conf)
    key = args.key or conf.get("seniverseKey") or ""
    location = args.location or conf.get("location") or "广州"
    if not key:
        log("没有心知私钥：在 %s 里填 seniverseKey" % args.conf)
        return False

    try:
        now_r = fw.api_get("now", key, location)
        try:
            daily_r = fw.api_get("daily", key, location, start=0, days=3)
        except Exception as e:
            log("预报接口失败，只写实况：%s" % e)
            daily_r = {"daily": []}
    except Exception as e:
        log("抓取失败（保留旧数据）：%s" % e)
        return False

    new = fw.build_cache(now_r, daily_r, 3)
    fp_new = fingerprint(new)

    old = ""
    if os.path.exists(args.out):
        try:
            old = open(args.out, encoding="utf-8").read()
        except OSError:
            old = ""
    fp_local = fingerprint(old) if old else ""

    # 上一次"确认推成功"的指纹。没有这个文件 = 没推过（或推失败过），必须推一次。
    # 这样板子刚刷过机、或上一轮 ssh 失败，下一轮都能自动补上。
    fp_pushed = ""
    if os.path.exists(args.pushed_mark):
        try:
            fp_pushed = open(args.pushed_mark, encoding="utf-8").read().strip()
        except OSError:
            fp_pushed = ""

    city = (now_r.get("location") or {}).get("name", location)
    now = now_r.get("now", {})
    hum = (daily_r.get("daily") or [{}])[0].get("humidity", "?")

    if fp_local == fp_new and fp_pushed == fp_new:
        log("%s 无变化（%s），跳过" % (city, str(now_r.get("last_update", ""))[:16]))
        return False

    if fp_local != fp_new:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(new)
        log("%s %s %s°C 湿度%s%% -> 已写入 %s" %
            (city, now.get("text", "?"), now.get("temperature", "?"), hum, args.out))

    if args.push:
        try:
            fw.push(new, args.board, args.dest)
            with open(args.pushed_mark, "w", encoding="utf-8") as f:
                f.write(fp_new)
            log("已推送 -> %s:%s" % (args.board, args.dest))
        except Exception as e:
            log("推送失败（下轮重试）：%s" % e)
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description="定时抓天气并推给开发板")
    ap.add_argument("--conf", default=fw.DEFAULT_CONF)
    ap.add_argument("--key", default=os.environ.get("WEATHER_SENIVERSE_KEY"))
    ap.add_argument("--location", default=os.environ.get("WEATHER_LOCATION"))
    ap.add_argument("--out", default="weather_cache.txt")
    ap.add_argument("--pushed-mark", default=".weather_pushed",
                    help="记录上一次推成功的指纹，用来判断要不要补推")
    ap.add_argument("--push", dest="push", action="store_true", default=True)
    ap.add_argument("--no-push", dest="push", action="store_false")
    ap.add_argument("--board", default="root@172.20.120.220")
    ap.add_argument("--dest", default="/root/lvgl-weather/weather_cache.txt")
    ap.add_argument("--interval", type=int, default=300, help="轮询间隔（秒），默认 300")
    ap.add_argument("--once", action="store_true", help="只跑一轮")
    args = ap.parse_args()

    if args.once:
        return 0 if run_once(args) else 0

    log("开始轮询：每 %d 秒一轮，城市=%s" % (args.interval, args.location or "（取配置）"))
    while True:
        try:
            run_once(args)
        except Exception as e:                      # 任何异常都不能让守护进程死掉
            log("本轮异常：%s" % e)
        time.sleep(max(30, args.interval))


if __name__ == "__main__":
    sys.exit(main())
