#!/usr/bin/env python3
"""M10 对照实验的下载源：GET /<n>m 返回 n MB 的零字节（n ≤ 64）。

只监听给定地址，可重复 --listen，例如
  python3 experiments/compare_data_server.py --listen 172.16.13.1:18200 --listen 127.0.0.1:18200
虚拟机经 ens192 访问 172.16.13.1；Mac 上的容器经 host.docker.internal 访问 127.0.0.1。
"""
import argparse
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHUNK = b"\0" * 65536


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        m = re.fullmatch(r"/(\d+)m", self.path)
        if not m or not 0 < int(m.group(1)) <= 64:
            self.send_error(404)
            return
        size = int(m.group(1)) << 20
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        left = size
        try:
            while left > 0:
                n = min(left, len(CHUNK))
                self.wfile.write(CHUNK[:n])
                left -= n
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, fmt, *args):  # 不打每个请求
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", action="append", required=True, help="host:port，可重复")
    a = ap.parse_args()
    servers = []
    for addr in a.listen:
        host, port = addr.rsplit(":", 1)
        s = ThreadingHTTPServer((host, int(port)), Handler)
        s.daemon_threads = True
        servers.append(s)
        threading.Thread(target=s.serve_forever, daemon=True).start()
        print("监听", addr, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    for s in servers:
        s.shutdown()


if __name__ == "__main__":
    main()
