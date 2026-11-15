import asyncio
import json
from types import SimpleNamespace

import httpx
import openai
import pytest

from lmai.context import Anonymizer
from lmai.llm import LlmClient, Selection, check_status
from lmai.schema import parse_report

GOOD = {"severity": "high", "summary": "CPU 被本地进程占满",
        "causes": [{"cause": "本地 stress 进程", "evidence": ["cpu 0.98，中位数 0.03"], "confidence": 0.8}],
        "actions": ["登录 lm-worker-1 查看 top"]}


class Tokens:
    def __init__(self, used=0):
        self.n = used

    async def used_today(self):
        return self.n

    async def add(self, n):
        self.n += n


def resp(content, tokens=100):
    return SimpleNamespace(choices=[SimpleNamespace(message=SimpleNamespace(content=content))],
                           usage=SimpleNamespace(total_tokens=tokens))


class FakeClient:
    """按脚本返回：字符串 → 正常响应；异常实例 → 抛出；'hang' → 永不返回。"""

    def __init__(self, script):
        self.script = list(script)
        self.calls = []
        self.chat = SimpleNamespace(completions=SimpleNamespace(create=self.create))

    async def create(self, **kw):
        self.calls.append(kw)
        step = self.script.pop(0)
        if step == "hang":
            await asyncio.sleep(3600)
        if isinstance(step, BaseException):
            raise step
        return resp(step)


def make(cfg, script, sel=("deepseek", "deepseek-chat"), used=0, monkeypatch=None):
    fc = FakeClient(script)
    sleeps = []

    async def sleep(s):
        sleeps.append(s)

    async def get_sel():
        return Selection(*sel)

    c = LlmClient(cfg.llm, get_sel, Tokens(used), factory=lambda v, k, t: fc, sleep=sleep)
    return c, fc, sleeps


@pytest.fixture(autouse=True)
def key(monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-test")


def test_parse_report_fence_and_errors():
    assert parse_report("```json\n" + json.dumps(GOOD) + "\n```").severity == "high"
    with pytest.raises(Exception):
        parse_report(json.dumps({**GOOD, "severity": "fatal"}))
    with pytest.raises(Exception):
        parse_report(json.dumps({**GOOD, "extra": 1}))
    with pytest.raises(Exception):
        parse_report(json.dumps({**GOOD, "causes": []}))


def test_status(cfg, monkeypatch):
    assert check_status(cfg.llm, Selection(), 0).reason == "disabled"
    assert check_status(cfg.llm, Selection("nope", "x"), 0).reason == "unknown_vendor"
    assert check_status(cfg.llm, Selection("deepseek", "gpt-4"), 0).reason == "unknown_model"
    assert check_status(cfg.llm, Selection("deepseek", "deepseek-chat"), 10**9).reason == "budget"
    assert check_status(cfg.llm, Selection("deepseek", "deepseek-chat"), 0).enabled
    monkeypatch.delenv("DEEPSEEK_API_KEY")
    assert check_status(cfg.llm, Selection("deepseek", "deepseek-chat"), 0).reason == "no_key"
    # 看板里填的 Key（BYOK）：没有 .env 也能启用；两者都有时用看板的
    st = check_status(cfg.llm, Selection("deepseek", "deepseek-chat", "sk-ui"), 0)
    assert st.enabled and st.key == "sk-ui"
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-env")
    assert check_status(cfg.llm, Selection("deepseek", "deepseek-chat", "sk-ui"), 0).key == "sk-ui"
    assert check_status(cfg.llm, Selection("deepseek", "deepseek-chat"), 0).key == "sk-env"


async def test_ok(cfg):
    c, fc, _ = make(cfg, [json.dumps(GOOD)])
    r = await c.diagnose("sys", "{}")
    assert r.reason == "ok" and r.report.summary == GOOD["summary"] and r.tokens == 100
    assert r.model == "deepseek/deepseek-chat" and r.attempts == 1
    assert fc.calls[0]["response_format"] == {"type": "json_object"}
    assert c.tokens.n == 100


async def test_invalid_then_fixed(cfg):
    c, fc, _ = make(cfg, ['{"severity": "bad"}', json.dumps(GOOD)])
    r = await c.diagnose("sys", "{}")
    assert r.reason == "ok" and r.attempts == 2 and r.tokens == 200
    assert "不符合要求" in fc.calls[1]["messages"][-1]["content"]


async def test_invalid_twice(cfg):
    c, _, _ = make(cfg, ["not json", "still not"])
    r = await c.diagnose("sys", "{}")
    assert r.report is None and r.reason == "invalid"


async def test_timeouts_retry_twice_then_give_up(cfg):
    cfg.llm.timeout_s = 0.05
    c, fc, sleeps = make(cfg, ["hang", "hang", "hang"])
    r = await c.diagnose("sys", "{}")
    assert r.report is None and r.reason.startswith("error: TimeoutError")
    assert len(fc.calls) == 3 and sleeps == [1.0, 2.0] and r.attempts == 3


async def test_retryable_then_ok(cfg):
    req = httpx.Request("POST", "http://x")
    err = openai.InternalServerError("boom", response=httpx.Response(502, request=req), body=None)
    c, fc, sleeps = make(cfg, [err, json.dumps(GOOD)])
    r = await c.diagnose("sys", "{}")
    assert r.reason == "ok" and sleeps == [1.0] and r.attempts == 2


async def test_non_retryable(cfg):
    req = httpx.Request("POST", "http://x")
    err = openai.AuthenticationError("bad key", response=httpx.Response(401, request=req), body=None)
    c, fc, sleeps = make(cfg, [err])
    r = await c.diagnose("sys", "{}")
    assert r.reason.startswith("error: AuthenticationError") and sleeps == [] and len(fc.calls) == 1


async def test_disabled_and_budget(cfg):
    c, fc, _ = make(cfg, [], sel=("", ""))
    assert (await c.diagnose("s", "u")).reason == "disabled" and fc.calls == []
    c, fc, _ = make(cfg, [], used=10**9)
    assert (await c.diagnose("s", "u")).reason == "budget" and fc.calls == []


def test_anonymizer_roundtrip():
    an = Anonymizer(["lm-worker-1", "lm-worker-10", "node-1"], True)
    text = an.hide({"node": "lm-worker-10", "peers": ["lm-worker-1", "node-1"], "metric": "nic.eth0.rtt_ms"})
    assert "lm-worker" not in text and "nic.eth0.rtt_ms" in text
    # 字典序：lm-worker-1 → node-1，lm-worker-10 → node-2，node-1 → node-3
    assert json.loads(text) == {"node": "node-2", "peers": ["node-1", "node-3"], "metric": "nic.eth0.rtt_ms"}
    back = an.reveal({"summary": "node-2 的 CPU 比 node-1、node-3 高", "x": ["node-2"]})
    assert back == {"summary": "lm-worker-10 的 CPU 比 lm-worker-1、node-1 高", "x": ["lm-worker-10"]}
    off = Anonymizer(["a"], False)
    assert off.hide({"n": "a"}) == '{"n":"a"}' and off.reveal("a") == "a"
