"""运维问答编排（M9）：函数调用循环 + SSE 事件。

一轮问答：把最近几轮对话和系统提示词交给模型（流式），模型要调工具就执行只读工具、把结果追加进对话再请求，
最多 max_tool_calls 次；达到上限后带 tool_choice="none" 再请求一次，强制成文。
产出 (事件名, 数据)：tool_call、tool_result、delta、done、error。
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from datetime import datetime
from typing import Any, AsyncIterator

from .config import Config
from .context import Anonymizer
from .llm import LlmClient, LlmUnavailable
from .tools import Tools, openai_tools, result_summary, truncate_result

log = logging.getLogger("lmai.chat")

CHAT_SYSTEM = """你是 Linux 服务器集群调度系统的运维助手，回答运维人员关于节点状态、分数、事件、任务和 AI 诊断的问题。

系统概况：Worker 每秒上报 CPU 忙碌度、调度延迟 P99、可用内存、磁盘利用率和各网卡 RTT/超时率/重传率；Manager 给每台节点打分
（score 0~1，越高越空闲），按 weight = score × f_inflight × f_timeout × f_slow × f_pred 加权随机把任务派给节点，prob 是选中概率。
节点状态：normal 正常、slow_start 慢启动（刚登记或刚恢复，权重从 0.2 升到 1）、evicted 剔除（不接任务）、rescue 急救。
f_inflight 在途任务多时下降；f_timeout = exp(−0.5 × T)，T 是半衰期 10 分钟的超时计数；f_pred 是 AI 负载预测因子（预测 30 秒后 CPU > 0.7 时 < 1）。

要求：
1. 先用工具查数据再回答，不要凭空猜测；工具没有返回的数据不要编造。需要多台节点时先调用 list_nodes。
2. 回答引用具体数值，带指标名和单位（CPU、内存是 0~1 的比例，可换算成百分数；调度延迟是微秒；完成时间是毫秒）。
3. 你只能读数据，不能剔除、重启或修改任何配置；用户要求操作时说明需要人工处理，并给出建议。
4. 用中文，简洁，先给结论再给依据。当前服务器本地时间：{now}。"""

VALID_ROLES = ("user", "assistant")


def clean_history(messages: Any, max_turns: int) -> list[dict[str, str]]:
    """只保留 user/assistant 的文本消息，取最近 max_turns 轮（2 × max_turns 条），最后一条必须是 user。"""
    if not isinstance(messages, list):
        raise ValueError("messages 必须是数组")
    out: list[dict[str, str]] = []
    for m in messages:
        if not isinstance(m, dict) or m.get("role") not in VALID_ROLES:
            continue
        c = m.get("content")
        if not isinstance(c, str) or not c.strip():
            continue
        out.append({"role": m["role"], "content": c[:4000]})
    out = out[-2 * max_turns:]
    while out and out[0]["role"] != "user":
        out.pop(0)
    if not out or out[-1]["role"] != "user":
        raise ValueError("最后一条消息必须是用户的问题")
    return out


async def run_chat(cfg: Config, llm: LlmClient, tools: Tools,
                   history: list[dict[str, str]]) -> AsyncIterator[tuple[str, dict[str, Any]]]:
    t0 = time.monotonic()
    names = await tools.known_nodes()
    anon = Anonymizer(names, cfg.diagnosis.anonymize)
    msgs: list[dict[str, Any]] = [
        {"role": "system", "content": CHAT_SYSTEM.format(now=datetime.now().strftime("%Y-%m-%d %H:%M:%S"))}]
    for m in history:
        msgs.append({"role": m["role"], "content": anon.hide_text(m["content"])})
    calls_used, tokens, model = 0, 0, None
    deadline = t0 + cfg.chat.timeout_s
    spec = openai_tools()
    try:
        while True:
            force = calls_used >= cfg.chat.max_tool_calls
            end: dict[str, Any] | None = None
            agen = llm.stream_chat(msgs, tools=spec, tool_choice="none" if force else None,
                                   max_tokens=cfg.chat.max_tokens)
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    raise asyncio.TimeoutError
                try:
                    ev = await asyncio.wait_for(agen.__anext__(), left)
                except StopAsyncIteration:
                    break
                if ev["type"] == "delta":
                    text = anon.reveal(ev["text"]) if anon.enabled else ev["text"]
                    yield "delta", {"text": text}
                else:
                    end = ev
            if end is None:
                raise RuntimeError("模型没有返回结束标记")
            tokens += end["tokens"]
            model = end["model"]
            calls = end["tool_calls"] if not force else []
            if not calls:
                yield "done", {"model": model, "tokens": tokens, "tool_calls": calls_used,
                               "ms": int((time.monotonic() - t0) * 1000)}
                return
            msgs.append({"role": "assistant", "content": end["content"] or "",
                         "tool_calls": [{"id": c["id"], "type": "function",
                                         "function": {"name": c["name"], "arguments": c["arguments"] or "{}"}}
                                        for c in calls]})
            for c in calls:
                try:
                    args = json.loads(c["arguments"]) if c["arguments"].strip() else {}
                except json.JSONDecodeError:
                    args = c["arguments"]
                shown = anon.reveal(args) if anon.enabled else args
                yield "tool_call", {"id": c["id"], "name": c["name"], "args": shown}
                if calls_used >= cfg.chat.max_tool_calls:
                    res: dict[str, Any] = {"error": f"本轮工具调用已达上限 {cfg.chat.max_tool_calls} 次，请根据已有数据回答"}
                    ms = 0
                else:
                    t1 = time.monotonic()
                    res = await tools.call(c["name"], shown)
                    ms = int((time.monotonic() - t1) * 1000)
                    calls_used += 1
                yield "tool_result", {"id": c["id"], "name": c["name"], "ok": "error" not in res, "ms": ms,
                                      "summary": result_summary(c["name"], res)}
                text = truncate_result(res, cfg.chat.result_max_bytes)
                msgs.append({"role": "tool", "tool_call_id": c["id"],
                             "content": anon.hide_text(text)})
    except LlmUnavailable as e:
        yield "error", {"reason": e.reason}
    except asyncio.TimeoutError:
        yield "error", {"reason": f"error: 超过 {cfg.chat.timeout_s:.0f} 秒没有完成"}
    except Exception as e:
        log.warning("问答出错：%s: %s", type(e).__name__, e)
        yield "error", {"reason": f"error: {type(e).__name__}: {e}"[:300]}


def sse(event: str, data: dict[str, Any]) -> bytes:
    return f"event: {event}\ndata: {json.dumps(data, ensure_ascii=False, default=str)}\n\n".encode()
