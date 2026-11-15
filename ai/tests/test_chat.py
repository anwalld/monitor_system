"""问答编排（M9）：事件顺序、工具上限后强制成文、未启用、token 计入预算、SSE 接口。"""

import json
from types import SimpleNamespace

from fastapi.testclient import TestClient

from fakes import FakeMySQL, FakeRedis
from lmai.api import create_app
from lmai.chat import clean_history, run_chat
from lmai.detector import Detector
from lmai.diagnosis import Service
from lmai.llm import LlmClient, Selection
from lmai.tools import Tools


def chunk(text=None, calls=None, finish=None, usage=None):
    delta = SimpleNamespace(content=text, tool_calls=calls)
    ch = SimpleNamespace(delta=delta, finish_reason=finish)
    return SimpleNamespace(choices=[ch], usage=SimpleNamespace(total_tokens=usage) if usage else None)


def call_chunk(i, cid, name, args_parts):
    out = [chunk(calls=[SimpleNamespace(index=i, id=cid, function=SimpleNamespace(name=name, arguments=""))])]
    for a in args_parts:
        out.append(chunk(calls=[SimpleNamespace(index=i, id=None, function=SimpleNamespace(name=None, arguments=a))]))
    return out


class StreamClient:
    """按脚本返回流：每一轮是 chunk 列表。"""

    def __init__(self, rounds):
        self.rounds = list(rounds)
        self.calls = []
        self.chat = SimpleNamespace(completions=SimpleNamespace(create=self.create))

    async def create(self, **kw):
        self.calls.append(kw)
        chunks = self.rounds.pop(0)

        async def gen():
            for c in chunks:
                yield c
        return gen()


def setup(cfg, monkeypatch, rounds, sel=("deepseek", "deepseek-chat")):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-test")
    r = FakeRedis(hashes={"w1": {"state": "normal", "score": "0.9", "prob": "1.0"}})
    r.settings = {"vendor": sel[0], "model": sel[1]}
    sc = StreamClient(rounds)
    llm = LlmClient(cfg.llm, r.get_selection, r, factory=lambda v, k, t: sc)
    tools = Tools(cfg, r, FakeMySQL(nodes=["w1"]))
    return llm, tools, sc, r


async def collect(cfg, llm, tools, q="哪台最闲？"):
    return [ev async for ev in run_chat(cfg, llm, tools, [{"role": "user", "content": q}])]


async def test_tool_then_answer(cfg, monkeypatch):
    rounds = [call_chunk(0, "c1", "list_nodes", ["{", "}"]) + [chunk(finish="tool_calls", usage=50)],
              [chunk("w1 "), chunk("最闲"), chunk(finish="stop", usage=80)]]
    llm, tools, sc, r = setup(cfg, monkeypatch, rounds)
    evs = await collect(cfg, llm, tools)
    kinds = [e for e, _ in evs]
    assert kinds == ["tool_call", "tool_result", "delta", "delta", "done"]
    assert evs[0][1]["name"] == "list_nodes" and evs[1][1]["ok"] and "1 台节点" in evs[1][1]["summary"]
    assert evs[-1][1] == {**evs[-1][1], "tokens": 130, "tool_calls": 1, "model": "deepseek/deepseek-chat"}
    assert r.tokens == 130
    # 第二轮请求里带上了 assistant tool_calls 和 tool 结果
    msgs = sc.calls[1]["messages"]
    assert msgs[-2]["tool_calls"][0]["function"]["name"] == "list_nodes"
    assert msgs[-1]["role"] == "tool" and json.loads(msgs[-1]["content"])["count"] == 1
    assert sc.calls[0]["stream"] is True and sc.calls[0]["tools"][0]["type"] == "function"


async def test_tool_limit_forces_answer(cfg, monkeypatch):
    cfg.chat.max_tool_calls = 2
    one = call_chunk(0, "a", "list_nodes", ["{}"]) + [chunk(finish="tool_calls", usage=10)]
    two = (call_chunk(0, "b", "list_nodes", ["{}"]) + call_chunk(1, "c", "explain_score", ['{"node":"w1"}'])
           + [chunk(finish="tool_calls", usage=10)])
    final = [chunk("够了"), chunk(finish="stop", usage=10)]
    llm, tools, sc, _ = setup(cfg, monkeypatch, [one, two, final])
    evs = await collect(cfg, llm, tools)
    results = [d for e, d in evs if e == "tool_result"]
    assert [x["ok"] for x in results] == [True, True, False]   # 第 3 次超过上限，返回错误给模型
    assert sc.calls[2]["tool_choice"] == "none"
    assert evs[-1][0] == "done" and evs[-1][1]["tool_calls"] == 2


async def test_disabled_and_budget(cfg, monkeypatch):
    llm, tools, sc, r = setup(cfg, monkeypatch, [], sel=("", ""))
    assert await collect(cfg, llm, tools) == [("error", {"reason": "disabled"})]
    llm, tools, sc, r = setup(cfg, monkeypatch, [])
    r.tokens = 10 ** 9
    assert await collect(cfg, llm, tools) == [("error", {"reason": "budget"})] and sc.calls == []


def test_clean_history():
    h = clean_history([{"role": "system", "content": "x"}, {"role": "assistant", "content": "hi"},
                       {"role": "user", "content": "q1"}, {"role": "assistant", "content": "a1"},
                       {"role": "user", "content": "q2"}], 1)
    assert h == [{"role": "user", "content": "q2"}]
    for bad in (None, [], [{"role": "assistant", "content": "x"}]):
        try:
            clean_history(bad, 10)
            raise AssertionError("应当拒绝")
        except ValueError:
            pass


def test_sse_endpoint(cfg, monkeypatch):
    rounds = [[chunk("你好"), chunk(finish="stop", usage=5)]]
    llm, tools, sc, r = setup(cfg, monkeypatch, rounds)
    svc = Service(cfg, r, FakeMySQL(), llm, Detector(cfg.detect))
    svc.tools = tools
    c = TestClient(create_app(svc))
    resp = c.post("/api/ai/chat", json={"messages": [{"role": "user", "content": "hi"}]})
    assert resp.status_code == 200 and resp.headers["content-type"].startswith("text/event-stream")
    assert "event: delta\ndata: {\"text\": \"你好\"}" in resp.text and "event: done" in resp.text
    assert c.post("/api/ai/chat", json={"messages": []}).status_code == 400
    cfg.api_token = "tk"
    assert c.post("/api/ai/chat", json={"messages": [{"role": "user", "content": "hi"}]}).status_code == 401
