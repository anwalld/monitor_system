#!/usr/bin/env python3
"""模拟 OpenAI 兼容的 /chat/completions，用于 AI 服务联调和故障注入（只用标准库）。

用法：python3 experiments/mock_llm.py --listen 127.0.0.1:18100 [--mode ok]
模式（也可运行中 POST /mode 正文为模式名切换）：
  ok            根据上下文里的异常生成合格 JSON
  invalid_once  每个诊断第一次返回不合格 JSON，修正请求返回合格 JSON
  invalid       总是不合格
  hang          不响应（测超时）
  error         返回 500
统计：GET /stats → 请求数、各模式计数。
"""

import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"mode": "ok", "requests": 0, "by_mode": {}}
LOCK = threading.Lock()


def build_report(user_msg: str) -> dict:
    try:
        ctx = json.loads(user_msg)
    except json.JSONDecodeError:
        ctx = {}
    a = ctx.get("anomaly", {})
    label = a.get("label") or a.get("metric") or "?"
    ev = []
    if a.get("value") is not None:
        ev.append(f"{label} 当前 {a['value']}，中位数 {a.get('median')}，z {a.get('z')}")
    peer = ctx.get("peer_median", {})
    if peer.get("nodes"):
        ev.append(f"其他 {peer['nodes']} 台节点 cpu 中位数 {peer.get('cpu')}")
    if not ev:
        ev.append(f"事件 {a.get('metric')}：{json.dumps(a.get('detail', {}), ensure_ascii=False)[:80]}")
    return {
        "severity": a.get("severity", "medium"),
        "summary": f"[mock] {a.get('node', '?')} 的 {label} 异常",
        "causes": [{"cause": f"[mock] {label} 偏离基线", "evidence": ev, "confidence": 0.5}],
        "actions": ["[mock] 登录节点检查"],
    }


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/stats"):
            with LOCK:
                return self._json(200, STATE)
        self._json(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n)
        if self.path.startswith("/mode"):
            with LOCK:
                STATE["mode"] = raw.decode().strip()
            return self._json(200, {"mode": STATE["mode"]})
        if not self.path.endswith("/chat/completions"):
            return self._json(404, {"error": "not found"})
        req = json.loads(raw or b"{}")
        msgs = req.get("messages", [])
        with LOCK:
            mode = STATE["mode"]
            STATE["requests"] += 1
            STATE["by_mode"][mode] = STATE["by_mode"].get(mode, 0) + 1
        if mode == "hang":
            time.sleep(3600)
            return
        if mode == "error":
            return self._json(500, {"error": {"message": "mock error"}})
        user = next((m["content"] for m in msgs if m["role"] == "user"), "")
        is_fix = sum(1 for m in msgs if m["role"] == "user") > 1
        if mode == "invalid" or (mode == "invalid_once" and not is_fix):
            content = '{"severity": "critical", "summary": ""}'
        else:
            content = json.dumps(build_report(user), ensure_ascii=False)
        self._json(200, {
            "id": "mock-1", "object": "chat.completion", "created": int(time.time()), "model": req.get("model"),
            "choices": [{"index": 0, "finish_reason": "stop", "message": {"role": "assistant", "content": content}}],
            "usage": {"prompt_tokens": len(user) // 3, "completion_tokens": len(content) // 3,
                      "total_tokens": len(user) // 3 + len(content) // 3},
        })


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:18100")
    ap.add_argument("--mode", default="ok")
    a = ap.parse_args()
    STATE["mode"] = a.mode
    host, _, port = a.listen.rpartition(":")
    srv = ThreadingHTTPServer((host, int(port)), H)
    srv.daemon_threads = True
    print(f"mock LLM 监听 {a.listen}，模式 {a.mode}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
