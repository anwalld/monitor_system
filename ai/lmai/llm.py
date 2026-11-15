"""LLM 调用：厂商选择、超时与重试、token 预算、JSON 校验重试。"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
import json
from typing import Any, AsyncIterator, Awaitable, Callable, Protocol

import openai
from pydantic import ValidationError

from .config import LlmCfg, VendorCfg
from .schema import Report, parse_report

log = logging.getLogger("lmai.llm")

RETRYABLE = (openai.APITimeoutError, openai.APIConnectionError, openai.RateLimitError,
             openai.InternalServerError)


class TokenCounter(Protocol):
    async def used_today(self) -> int: ...
    async def add(self, n: int) -> None: ...


@dataclass
class Selection:
    vendor: str = ""
    model: str = ""
    ui_key: str = ""   # 看板里填的 Key（Redis ai:keys），优先于 .env


def effective_key(v: VendorCfg, ui_key: str) -> str:
    return ui_key.strip() or v.api_key()


@dataclass
class Status:
    enabled: bool
    reason: str               # ok / disabled / unknown_vendor / unknown_model / no_key / budget
    vendor: VendorCfg | None = None
    model: str = ""
    key: str = ""


@dataclass
class LlmResult:
    report: Report | None
    reason: str               # ok / disabled / no_key / budget / invalid / error: ...
    model: str | None = None  # 厂商/模型
    tokens: int = 0
    attempts: int = 0


def check_status(cfg: LlmCfg, sel: Selection, used_today: int | None) -> Status:
    if not sel.vendor:
        return Status(False, "disabled")
    v = cfg.vendors.get(sel.vendor)
    if v is None:
        return Status(False, "unknown_vendor")
    if sel.model not in v.models:
        return Status(False, "unknown_model", v, sel.model)
    key = effective_key(v, sel.ui_key)
    if not key:
        return Status(False, "no_key", v, sel.model)
    if used_today is not None and used_today >= cfg.daily_token_budget:
        return Status(False, "budget", v, sel.model, key)
    return Status(True, "ok", v, sel.model, key)


class LlmUnavailable(Exception):
    """LLM 未启用、缺 Key、超预算（reason 与诊断相同：disabled / no_key / budget / …）。"""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


ClientFactory = Callable[[VendorCfg, str, float], Any]


def default_factory(v: VendorCfg, key: str, timeout_s: float) -> Any:
    return openai.AsyncOpenAI(base_url=v.base_url, api_key=key, timeout=timeout_s, max_retries=0)


class LlmClient:
    def __init__(self, cfg: LlmCfg, get_selection: Callable[[], Awaitable[Selection]],
                 tokens: TokenCounter, factory: ClientFactory = default_factory,
                 sleep: Callable[[float], Awaitable[None]] = asyncio.sleep):
        self.cfg = cfg
        self.get_selection = get_selection
        self.tokens = tokens
        self.factory = factory
        self.sleep = sleep
        self._clients: dict[tuple[str, str, str], Any] = {}
        self.last: dict[str, Any] | None = None  # 最近一次真正发起的调用：ts_ms、model、reason（设置页显示）

    async def status(self) -> Status:
        sel = await self.get_selection()
        try:
            used = await self.tokens.used_today()
        except Exception:  # Redis 不可用时不因预算拦截，调用时再判断
            used = None
        return check_status(self.cfg, sel, used)

    def _client(self, v: VendorCfg, key: str) -> Any:
        k = (v.name, v.base_url, key)
        c = self._clients.get(k)
        if c is None:  # 换了 Key 就换客户端
            self._clients = {kk: val for kk, val in self._clients.items() if kk[0] != v.name}
            c = self._clients[k] = self.factory(v, key, self.cfg.timeout_s)
        return c

    async def _call(self, client: Any, model: str, messages: list[dict[str, Any]], json_mode: bool,
                    extra: dict[str, Any] | None = None) -> Any:
        """一次请求，超时、连接错误、429、5xx 按 1、2 秒退避重试 max_retries 次。"""
        kwargs: dict[str, Any] = dict(model=model, messages=messages, temperature=self.cfg.temperature,
                                      max_tokens=self.cfg.max_tokens)
        if json_mode:
            kwargs["response_format"] = {"type": "json_object"}
        kwargs.update(extra or {})
        attempt = 0
        while True:
            attempt += 1
            try:
                # 外层再套一层超时：SDK 的 timeout 只管单次 HTTP 读写
                return await asyncio.wait_for(client.chat.completions.create(**kwargs),
                                              self.cfg.timeout_s + min(5.0, self.cfg.timeout_s)), attempt
            except (asyncio.TimeoutError, *RETRYABLE) as e:
                if attempt > self.cfg.max_retries:
                    raise
                delay = self.cfg.backoff_s * (2 ** (attempt - 1))
                log.warning("LLM 请求失败（%s），%.0f 秒后第 %d 次重试", type(e).__name__, delay, attempt)
                await self.sleep(delay)

    async def diagnose(self, system: str, user: str) -> LlmResult:
        res = await self._diagnose(system, user)
        if res.model is not None:
            self.last = {"ts_ms": int(time.time() * 1000), "model": res.model, "reason": res.reason,
                         "tokens": res.tokens}
        return res

    async def _diagnose(self, system: str, user: str) -> LlmResult:
        st = await self.status()
        if not st.enabled:
            return LlmResult(None, st.reason)
        v, model = st.vendor, st.model
        assert v is not None
        name = f"{v.name}/{model}"
        client = self._client(v, st.key)
        messages = [{"role": "system", "content": system}, {"role": "user", "content": user}]
        total_tokens, attempts, fixed = 0, 0, False
        while True:
            try:
                if await self.tokens.used_today() >= self.cfg.daily_token_budget:
                    return LlmResult(None, "budget", name, total_tokens, attempts)
            except Exception:
                pass
            try:
                resp, n = await self._call(client, model, messages, v.json_mode)
            except Exception as e:  # 重试用尽或不可重试的错误
                attempts += self.cfg.max_retries + 1 if isinstance(e, (asyncio.TimeoutError, *RETRYABLE)) else 1
                msg = f"{type(e).__name__}: {e}"[:300]
                log.warning("LLM 调用放弃：%s", msg)
                return LlmResult(None, "error: " + msg, name, total_tokens, attempts)
            attempts += n
            used = int(getattr(getattr(resp, "usage", None), "total_tokens", 0) or 0)
            total_tokens += used
            try:
                await self.tokens.add(used)
            except Exception:
                pass
            text = resp.choices[0].message.content if resp.choices else None
            try:
                return LlmResult(parse_report(text), "ok", name, total_tokens, attempts)
            except (ValueError, ValidationError) as e:
                err = str(e)[:800]
                if fixed:
                    log.warning("LLM 输出两次不合格：%s", err[:200])
                    return LlmResult(None, "invalid", name, total_tokens, attempts)
                fixed = True
                messages += [
                    {"role": "assistant", "content": text or ""},
                    {"role": "user", "content": f"上面的输出不符合要求：{err}\n请修正后只输出一个符合格式的 JSON 对象。"},
                ]

    # ---------- M9：日报（纯文本）与问答（流式 + 工具调用） ----------
    async def _enabled(self) -> Status:
        st = await self.status()
        if not st.enabled:
            raise LlmUnavailable(st.reason)
        return st

    def _record(self, model: str, reason: str, tokens: int) -> None:
        self.last = {"ts_ms": int(time.time() * 1000), "model": model, "reason": reason, "tokens": tokens}

    async def complete(self, system: str, user: str, max_tokens: int | None = None) -> tuple[str, LlmResult]:
        """一次非流式纯文本调用。返回 (正文, 结果)；不可用时 LlmUnavailable，调用失败时正文为空、reason 为 error。"""
        st = await self._enabled()
        assert st.vendor is not None
        name = f"{st.vendor.name}/{st.model}"
        client = self._client(st.vendor, st.key)
        messages = [{"role": "system", "content": system}, {"role": "user", "content": user}]
        try:
            resp, n = await self._call(client, st.model, messages, False,
                                       {"max_tokens": max_tokens or self.cfg.max_tokens})
        except Exception as e:
            msg = f"{type(e).__name__}: {e}"[:300]
            self._record(name, "error: " + msg, 0)
            return "", LlmResult(None, "error: " + msg, name, 0, self.cfg.max_retries + 1)
        used = int(getattr(getattr(resp, "usage", None), "total_tokens", 0) or 0)
        try:
            await self.tokens.add(used)
        except Exception:
            pass
        self._record(name, "ok", used)
        text = resp.choices[0].message.content if resp.choices else ""
        return text or "", LlmResult(None, "ok", name, used, n)

    async def stream_chat(self, messages: list[dict[str, Any]], tools: list[dict[str, Any]] | None = None,
                          tool_choice: str | None = None, max_tokens: int | None = None) -> AsyncIterator[dict[str, Any]]:
        """流式调用。产出 {"type": "delta", "text"}……，最后一条
        {"type": "end", "content", "tool_calls": [{"id","name","arguments"}], "finish_reason", "tokens", "model"}。
        建立连接前的超时、连接错误、429、5xx 按退避重试；开始输出后出错直接抛出。"""
        st = await self._enabled()
        try:
            if await self.tokens.used_today() >= self.cfg.daily_token_budget:
                raise LlmUnavailable("budget")
        except LlmUnavailable:
            raise
        except Exception:
            pass
        assert st.vendor is not None
        name = f"{st.vendor.name}/{st.model}"
        client = self._client(st.vendor, st.key)
        kwargs: dict[str, Any] = dict(model=st.model, messages=messages, temperature=self.cfg.temperature,
                                      max_tokens=max_tokens or self.cfg.max_tokens, stream=True,
                                      stream_options={"include_usage": True})
        if tools:
            kwargs["tools"] = tools
            if tool_choice:
                kwargs["tool_choice"] = tool_choice
        timeout = self.cfg.timeout_s
        attempt = 0
        while True:
            attempt += 1
            try:
                stream = await asyncio.wait_for(client.chat.completions.create(**kwargs), timeout)
                break
            except (asyncio.TimeoutError, *RETRYABLE) as e:
                if attempt > self.cfg.max_retries:
                    self._record(name, f"error: {type(e).__name__}", 0)
                    raise
                delay = self.cfg.backoff_s * (2 ** (attempt - 1))
                log.warning("LLM 流式请求失败（%s），%.0f 秒后第 %d 次重试", type(e).__name__, delay, attempt)
                await self.sleep(delay)
        content: list[str] = []
        calls: dict[int, dict[str, str]] = {}
        finish, usage = None, 0
        it = stream.__aiter__()
        while True:
            try:
                chunk = await asyncio.wait_for(it.__anext__(), timeout)  # 每个分块最多等 timeout_s
            except StopAsyncIteration:
                break
            u = getattr(chunk, "usage", None)
            if u is not None and getattr(u, "total_tokens", None):
                usage = int(u.total_tokens)
            for ch in getattr(chunk, "choices", None) or []:
                d = ch.delta
                text = getattr(d, "content", None)
                if text:
                    content.append(text)
                    yield {"type": "delta", "text": text}
                for tc in getattr(d, "tool_calls", None) or []:
                    c = calls.setdefault(int(getattr(tc, "index", 0) or 0), {"id": "", "name": "", "arguments": ""})
                    if getattr(tc, "id", None):
                        c["id"] = tc.id
                    fn = getattr(tc, "function", None)
                    if fn is not None:
                        if getattr(fn, "name", None):
                            c["name"] = fn.name if not c["name"] else c["name"]
                        if getattr(fn, "arguments", None):
                            c["arguments"] += fn.arguments
                if getattr(ch, "finish_reason", None):
                    finish = ch.finish_reason
        text = "".join(content)
        if not usage:  # 厂商没返回 usage：按字符数估算
            usage = (len(json.dumps(messages, ensure_ascii=False)) + len(text)) // 3
        try:
            await self.tokens.add(usage)
        except Exception:
            pass
        self._record(name, "ok", usage)
        tool_calls = [dict(c, id=c["id"] or f"call_{i}") for i, c in sorted(calls.items()) if c["name"]]
        yield {"type": "end", "content": text, "tool_calls": tool_calls, "finish_reason": finish,
               "tokens": usage, "model": name}
