#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mqtts_relay.py —— 给"没有 libssl 也没有外网"的开发板当 MQTTS 网关。

    GEC6818  --明文 MQTT-->  本机 172.20.120.100:1883  --TLS-->  华为云 8883

为什么需要它：
  GEC6818 的 rootfs 里没有 libssl/libcrypto，编译 MQTTS 客户端要连带交叉编译
  OpenSSL；而且这块板子当前连默认路由都没有（更别说解析外网域名）。
  把 TLS 放到性能富余的电脑上终结，板子只说明文 MQTT —— 链路是加密的，
  只是加密段变成"本机到云"这一段。

用法：
    python3 weather/tools/mqtts_relay.py                      # 监听 0.0.0.0:1883
    python3 weather/tools/mqtts_relay.py --listen 172.20.120.100:1883 -v
    python3 weather/tools/mqtts_relay.py --target 4f18e1d861.st1.iotda-device.cn-south-1.myhuaweicloud.com:8883

装成开机自启（WSL 里）：
    crontab -e
    @reboot cd /home/user/code/lv_port_pc_vscode-release-v8 && python3 weather/tools/mqtts_relay.py >> /tmp/relay.log 2>&1
"""

import argparse
import socket
import ssl
import sys
import threading
import time

VERBOSE = False


def log(*a):
    if VERBOSE:
        print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def pump(src, dst, label):
    """把一个方向的字节搬完，然后半关闭对端。"""
    total = 0
    try:
        while True:
            data = src.recv(4096)
            if not data:
                break
            dst.sendall(data)
            total += len(data)
    except (OSError, ssl.SSLError) as e:
        log("%s 中断: %s" % (label, e))
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        log("%s 结束, %d 字节" % (label, total))


def handle(conn, addr, host, port, ctx):
    log("板子接入 %s:%s" % addr)
    try:
        raw = socket.create_connection((host, port), timeout=10)
        upstream = ctx.wrap_socket(raw, server_hostname=host)
    except (OSError, ssl.SSLError) as e:
        print("[relay] 连不上云端 %s:%d -> %s" % (host, port, e), flush=True)
        conn.close()
        return

    log("已与云端握手, 协议=%s" % upstream.version())
    t1 = threading.Thread(target=pump, args=(conn, upstream, "C->S"), daemon=True)
    t2 = threading.Thread(target=pump, args=(upstream, conn, "S->C"), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    try:
        conn.close()
    except OSError:
        pass
    try:
        upstream.close()
    except OSError:
        pass
    log("连接关闭 %s:%s" % addr)


def main():
    ap = argparse.ArgumentParser(description="明文 MQTT -> TLS -> 华为云 IoTDA 中转")
    ap.add_argument("--listen", default="0.0.0.0:1883", help="监听地址（板子连这里）")
    ap.add_argument("--target",
                    default="4f18e1d861.st1.iotda-device.cn-south-1.myhuaweicloud.com:8883",
                    help="云端 MQTTS 地址")
    ap.add_argument("--insecure", action="store_true", help="跳过对端证书校验（不推荐）")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    global VERBOSE
    VERBOSE = args.verbose

    lhost, lport = args.listen.rsplit(":", 1)
    host, port = args.target.rsplit(":", 1)
    port = int(port)

    ctx = ssl.create_default_context()
    if args.insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((lhost, int(lport)))
    srv.listen(8)
    print("[relay] 监听 %s:%s  ->  %s:%d (TLS)" % (lhost, lport, host, port), flush=True)

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle, args=(conn, addr, host, port, ctx),
                             daemon=True).start()
    except KeyboardInterrupt:
        print("\n[relay] 退出", flush=True)
    finally:
        srv.close()


if __name__ == "__main__":
    sys.exit(main())
