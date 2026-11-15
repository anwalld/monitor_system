#!/usr/bin/env python3
"""模拟 OpenAI 兼容的 /chat/completions，用于 AI 服务联调和故障注入（只用标准库）。

用法：python3 experiments/mock_llm.py --listen 127.0.0.1:18100 [--mode ok]
模式（也可运行中 POST /mode 正文为模式名切换）：
  ok            根据上下文里的异常生成合格 JSON
  invalid_once  每个诊断第一次返回不合格 JSON，修正请求返回合格 JSON
  invalid       总是不合格
  hang          不响应（测超时）
  error         返回 500
  report        日报：只引用 stats 里的数字（M9）
  report_bad    日报：故意写错一个数字，测核对（M9）
问答（M9）：请求带 tools 且对话里还没有 tool 消息时，先返回一次 list_nodes 的 tool_call，之后返回文本；
stream: true 时按 SSE 分块输出（data: {...} … data: [DONE]），带 usage。
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


def build_daily(user_msg: str, bad: bool) -> str:
    try:
        st = json.loads(user_msg)
    except json.JSONDecodeError:
        return "[mock] 日报"
    lines = [f"[mock] {st.get('day')} 日报"]
    for n in st.get("nodes", []):
        lines.append(f"- {n['node']}：可用率 {n.get('avail_pct')}%，剔除 {n.get('evictions')} 次，"
                     f"任务 {n.get('tasks')} 个，超时 {n.get('timeouts')} 个。")
    if bad:
        lines.append("- 超时率 12.345%。")
    return "\n".join(lines)


def chat_reply(msgs: list) -> dict:
    """问答：还没有工具结果时请求 list_nodes，否则根据工具结果回答。返回 message dict。"""
    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    if not tool_msgs:
        return {"role": "assistant", "content": "",
                "tool_calls": [{"id": "call_mock_1", "type": "function",
                                "function": {"name": "list_nodes", "arguments": "{}"}}]}
    try:
        res = json.loads(tool_msgs[-1]["content"])
    except json.JSONDecodeError:
        res = {}
    nodes = res.get("nodes") or []
    best = max(nodes, key=lambda n: n.get("prob") or 0, default={})
    text = f"[mock] 共 {res.get('count', len(nodes))} 台节点，选中概率最高的是 {best.get('node', '?')}（prob {best.get('prob')}）。"
    return {"role": "assistant", "content": text}


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
        if req.get("tools") and req.get("tool_choice") != "none":
            message = chat_reply(msgs)
        elif req.get("tools"):
            message = {"role": "assistant", "content": "[mock] 已达工具上限，按已有数据回答。"}
        elif mode in ("report", "report_bad"):
            message = {"role": "assistant", "content": build_daily(user, mode == "report_bad")}
        elif mode == "invalid" or (mode == "invalid_once" and not is_fix):
            message = {"role": "assistant", "content": '{"severity": "critical", "summary": ""}'}
        else:
            message = {"role": "assistant", "content": json.dumps(build_report(user), ensure_ascii=False)}
        content = message.get("content") or ""
        usage = {"prompt_tokens": len(raw) // 3, "completion_tokens": len(content) // 3 + 5,
                 "total_tokens": len(raw) // 3 + len(content) // 3 + 5}
        finish = "tool_calls" if message.get("tool_calls") else "stop"
        if req.get("stream"):
            return self._stream(req, message, finish, usage)
        self._json(200, {
            "id": "mock-1", "object": "chat.completion", "created": int(time.time()), "model": req.get("model"),
            "choices": [{"index": 0, "finish_reason": finish, "message": message}],
            "usage": usage,
        })

    def _stream(self, req, message, finish, usage):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        base = {"id": "mock-s", "object": "chat.completion.chunk", "created": int(time.time()),
                "model": req.get("model")}

        def send(delta, fin=None, use=None):
            obj = dict(base, choices=[{"index": 0, "delta": delta, "finish_reason": fin}])
            if use is not None:
                obj = dict(base, choices=[], usage=use)
            self.wfile.write(f"data: {json.dumps(obj, ensure_ascii=False)}\n\n".encode())
            self.wfile.flush()

        send({"role": "assistant", "content": ""})
        for i, tc in enumerate(message.get("tool_calls") or []):
            send({"tool_calls": [{"index": i, "id": tc["id"], "type": "function",
                                  "function": {"name": tc["function"]["name"], "arguments": ""}}]})
            send({"tool_calls": [{"index": i, "function": {"arguments": tc["function"]["arguments"]}}]})
        text = message.get("content") or ""
        for j in range(0, len(text), 6):  # 6 个字一块，模拟逐字输出
            send({"content": text[j:j + 6]})
            time.sleep(0.02)
        send({}, finish)
        if (req.get("stream_options") or {}).get("include_usage"):
            send(None, use=usage)
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


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
