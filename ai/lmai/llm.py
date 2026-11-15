"""LLM 调用：厂商选择、超时与重试、token 预算、JSON 校验重试。"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
from typing import Any, Awaitable, Callable, Protocol

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

    async def _call(self, client: Any, model: str, messages: list[dict[str, str]], json_mode: bool) -> Any:
        """一次请求，超时、连接错误、429、5xx 按 1、2 秒退避重试 max_retries 次。"""
        kwargs: dict[str, Any] = dict(model=model, messages=messages, temperature=self.cfg.temperature,
                                      max_tokens=self.cfg.max_tokens)
        if json_mode:
            kwargs["response_format"] = {"type": "json_object"}
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
