"""日报（M9）：统计口径、数字核对、表格回退、LLM 成文。"""

import json
from types import SimpleNamespace

import pytest

from fakes import FakeMySQL, FakeRedis
from lmai.llm import LlmClient
from lmai.report import Reporter, check_numbers, compute_stats, day_bounds, extract_numbers, render_table

DAY = "2026-11-15"
S, E = day_bounds(DAY)


def mysql():
    m5 = [{"node_id": "w1", "ts_ms": S + i * 300_000, "cpu_avg": 0.4, "cpu_max": 0.5 + (i == 10) * 0.49,
           "samples": 300 if i < 287 else 0} for i in range(288)]
    m5 += [{"node_id": "w1", "ts_ms": S - 300_000, "cpu_avg": 0.9, "cpu_max": 1.0, "samples": 300}]  # 前一天，不算
    events = [{"node": "w1", "ts_ms": S + 1000 * i, "kind": "evict", "detail": {}} for i in range(3)]
    events += [{"node": "w1", "ts_ms": S + 5000, "kind": "rescue_start", "detail": {}},
               {"node": "w1", "ts_ms": E + 1, "kind": "evict", "detail": {}}]
    tasks = [{"node_id": "w1", "state": "done", "exec_ms": 200.0 + i, "t_alloc": S + i} for i in range(1190)]
    tasks += [{"node_id": "w1", "state": "timeout", "exec_ms": None, "t_alloc": S + 5, "no_recv": i < 4} for i in range(10)]
    diags = [{"id": 1, "ts": S + 3_600_000, "node": "w1", "severity": "high", "summary": "CPU 满",
              "label": "CPU 忙碌度", "llm": "ok"}]
    return FakeMySQL(m5=m5, events=events, tasks=tasks, diags=diags)


async def test_compute_stats():
    st = await compute_stats(mysql(), DAY)
    (n,) = st["nodes"]
    assert n["avail_s"] == 86100 and n["avail_pct"] == 99.65 and n["avail_h"] == 23.92
    assert n["cpu_avg_pct"] == 40.0 and n["cpu_max_pct"] == 99.0
    assert n["evictions"] == 3 and n["rescues"] == 1
    assert n["tasks"] == 1200 and n["timeouts"] == 10 and n["done"] == 1190 and n["timeout_pct"] == 0.83
    assert n["unconfirmed"] == 4 and n["exec_timeouts"] == 6
    assert n["p50_ms"] == 794 and n["p99_ms"] == 1378 and n["max_ms"] == 1389
    t = st["totals"]
    assert t["nodes"] == 1 and t["tasks"] == 1200 and t["states"] == {"done": 1190, "timeout": 10}
    d = st["diagnoses"]
    assert d["total"] == 1 and d["high"] == 1 and d["by_node"] == {"w1": 1} and d["items"][0]["time"]
    assert st["triggers"] == [{"trigger": "CPU 忙碌度", "count": 1}]


async def test_check_numbers_rules():
    st = await compute_stats(mysql(), DAY)
    ok = ("# 2026-11-15 日报\n\n1. w1 可用率 99.65%（约 23.9 小时），不可用 0.35%；剔除 3 次、急救 1 次。\n"
          "2. 任务 1,200 个，超时 10 个（0.83%），P50 794 ms、P99 1.4 秒。CPU 平均 40%，09:00 前后一条高严重度诊断。")
    assert check_numbers(ok, st) == []
    assert check_numbers("超时率 0.84%，任务 1201 个", st) == ["0.84", "1201"]
    assert extract_numbers("lm-worker-1 和 ens160 的 P99", ["lm-worker-1"]) == []


async def test_generate_ok_mismatch_and_disabled(cfg, monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk")
    r = FakeRedis()
    replies = []

    class C:
        def __init__(self):
            self.chat = SimpleNamespace(completions=SimpleNamespace(create=self.create))

        async def create(self, **kw):
            text = replies.pop(0)
            return SimpleNamespace(choices=[SimpleNamespace(message=SimpleNamespace(content=text))],
                                   usage=SimpleNamespace(total_tokens=300))

    llm = LlmClient(cfg.llm, r.get_selection, r, factory=lambda v, k, t: C())
    m = mysql()
    rep = Reporter(cfg, m, llm)

    # 未启用 LLM：表格
    out = await rep.generate(DAY)
    assert out["stats"]["llm"] == "disabled" and "| w1 | 99.65 |" in out["markdown"]
    assert check_numbers(out["markdown"], out["stats"]) == []          # 表格里的数字本身一致
    r.settings = {"vendor": "deepseek", "model": "deepseek-chat"}
    replies.append("w1 可用率 99.65%，剔除 3 次。")
    out = await rep.generate(DAY)
    assert out["stats"]["llm"] == "ok" and out["markdown"].startswith("w1 可用率")
    assert out["stats"]["tokens"] == 300 and m.reports[DAY]["markdown"] == out["markdown"]
    replies.append("w1 可用率 98.1%。")
    out = await rep.generate(DAY)
    assert out["stats"]["llm"] == "mismatch" and out["stats"]["mismatched"] == ["98.1"]
    assert "对不上统计" in out["markdown"] and "| w1 |" in out["markdown"]
    assert rep.health()["last"]["llm"] == "mismatch"


async def test_catch_up_only_after_time(cfg):
    import time as _t
    from datetime import datetime
    m = FakeMySQL()
    r = FakeRedis()
    llm = LlmClient(cfg.llm, r.get_selection, r)
    at_10 = datetime(2026, 11, 16, 10, 0).timestamp()
    at_8 = datetime(2026, 11, 16, 8, 0).timestamp()
    rep = Reporter(cfg, m, llm, clock=lambda: at_8)
    await rep.catch_up()
    assert m.reports == {}
    rep = Reporter(cfg, m, llm, clock=lambda: at_10)
    await rep.catch_up()
    assert list(m.reports) == ["2026-11-15"]
    assert render_table({"day": DAY, "nodes": [], "totals": {}, "diagnoses": {}}).startswith("# 2026-11-15")
    assert _t
