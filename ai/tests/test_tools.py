"""运维问答工具（M9）：用假 Redis/MySQL 覆盖六个工具、未知节点、参数限幅、降采样点数。"""

import json

import pytest

from fakes import FakeMySQL, FakeRedis
from lmai.tools import SPECS, Tools, bucketize, quantile, result_summary, truncate_result

NOW = 1_800_000_000_000


def mk(cfg, **kw):
    hashes = {
        "w1": {"state": "normal", "score": "0.8200", "s_cpu": "0.9000", "s_runq": "0.8000", "s_mem": "0.7000",
               "s_net": "0.8500", "prob": "0.6000", "cpu_busy": "0.1500", "f_inflight": "1.0000",
               "f_timeout": "1.0000", "f_slow": "1.0000", "f_pred": "0.7500", "last_seen_ms": str(NOW - 500),
               "evicted": "0", "best_nic": "eth0"},
        "w2": {"state": "evicted", "state_reason": "lost", "score": "0.3000", "prob": "0.0000",
               "last_seen_ms": str(NOW - 60000)},
    }
    ts = {"w1": [{"node_id": "w1", "ts_ms": str(NOW - 600_000 + i * 1000), "cpu_busy": 0.1 + (i % 10) / 100,
                  "mem_avail_ratio": 0.8, "runq_p99_us": 100 + i,
                  "nics": [{"name": "eth0", "rtt_ms": 1.0}, {"name": "eth1", "rtt_ms": 2.0, "timeout_rate": 0.5}]}
                 for i in range(600)]}
    r = FakeRedis(hashes=hashes, ts=ts, preds={"w1": {"cpu_busy_30s": 0.85, "factor": 0.75}},
                  events=[{"node": "w2", "kind": "evict", "ts_ms": NOW - 1000, "detail": {"reason": "lost"}}])
    m = FakeMySQL(nodes=["w1", "w2", "old"], **kw)
    return Tools(cfg, r, m, clock=lambda: NOW / 1000), r, m


def test_specs_are_valid_and_closed():
    assert {s["name"] for s in SPECS} == {"list_nodes", "get_node_metrics", "get_node_events", "explain_score",
                                          "get_task_stats", "get_diagnoses"}
    for s in SPECS:
        assert s["parameters"]["type"] == "object" and s["parameters"]["additionalProperties"] is False


async def test_list_nodes(cfg):
    t, _, _ = mk(cfg)
    r = await t.call("list_nodes", "")
    assert r["count"] == 2 and r["online"] == 1
    w1 = r["nodes"][0]
    assert w1["node"] == "w1" and w1["score"] == 0.82 and w1["f_pred"] == 0.75 and w1["online"]
    assert w1["predict"]["factor"] == 0.75
    assert r["nodes"][1]["state"] == "evicted" and r["nodes"][1]["last_report_age_s"] == 60.0


async def test_metrics_redis_downsampled_and_nic(cfg):
    t, _, _ = mk(cfg)
    r = await t.call("get_node_metrics", {"node": "w1", "metric": "cpu", "minutes": 10})
    pts = r["series"]["cpu"]["points"]
    assert r["step_s"] == 10 and 55 <= len(pts) <= 61
    assert r["series"]["cpu"]["min"] >= 0.1 and r["series"]["cpu"]["max"] <= 0.2
    r = await t.call("get_node_metrics", {"node": "w1", "metric": "timeout_rate", "minutes": 5})
    assert set(r["series"]) == {"eth0", "eth1"}             # 按网卡分列，省略的 0 补回
    assert r["series"]["eth0"]["max"] == 0 and r["series"]["eth1"]["avg"] == 0.5
    r = await t.call("get_node_metrics", {"node": "w1", "metric": "runq_p99_us", "minutes": 10})
    assert r["agg"] == "桶内最大"


async def test_metrics_mysql_for_long_ranges(cfg):
    m5 = [{"node_id": "w1", "ts_ms": NOW - 86_400_000 + i * 300_000, "cpu_avg": 0.2, "cpu_max": 0.5 + i / 1000,
           "mem_avail_min": 0.7, "samples": 300} for i in range(288)]
    t, _, _ = mk(cfg, m5=m5)
    r = await t.call("get_node_metrics", {"node": "w1", "metric": "cpu", "minutes": 1440})
    assert "MySQL" in r["source"] and len(r["series"]["cpu"]["points"]) <= 60
    assert r["series"]["cpu_max"]["max"] == pytest.approx(0.787)
    r = await t.call("get_node_metrics", {"node": "w1", "metric": "disk", "minutes": 120})
    assert "error" in r                                      # 聚合里没有 disk


async def test_unknown_node_metric_and_params(cfg):
    t, _, _ = mk(cfg)
    r = await t.call("get_node_metrics", {"node": "nope", "metric": "cpu", "minutes": 10})
    assert r["error"] == "未知节点 nope" and "w1" in r["nodes"]
    assert "未知指标" in (await t.call("get_node_metrics", {"node": "w1", "metric": "foo"}))["error"]
    assert "未知参数" in (await t.call("list_nodes", {"x": 1}))["error"]
    assert "没有工具" in (await t.call("rm_rf", {}))["error"]
    assert "JSON" in (await t.call("explain_score", "{bad"))["error"]
    # minutes 越界限幅到 1440，不报错
    r = await t.call("get_node_metrics", {"node": "w1", "metric": "mem_avail", "minutes": 99999})
    assert "MySQL" in r["source"]


async def test_events_from_mysql_and_fallback(cfg):
    t, _, m = mk(cfg, events=[{"node": "w2", "ts_ms": NOW - 3_600_000, "kind": "evict", "detail": {"reason": "probe"}},
                              {"node": "w2", "ts_ms": NOW - 7_200_000, "kind": "slow_start", "detail": {}}])
    r = await t.call("get_node_events", {"node": "w2", "hours": 24})
    assert r["count"] == 2 and r["by_kind"] == {"evict": 1, "slow_start": 1} and r["events"][0]["kind"] == "evict"
    m.down = True
    r = await t.call("get_node_events", {"node": "w2", "hours": 1})
    assert "Redis" in r["source"] and r["events"][0]["detail"] == {"reason": "lost"}


async def test_explain_score(cfg):
    t, _, _ = mk(cfg)
    r = await t.call("explain_score", {"node": "w1"})
    assert r["parts"]["cpu"] == {"score": 0.9, "weight": 0.3}
    assert r["score_recomputed"] == pytest.approx(0.82, abs=1e-4) and "warning" not in r
    assert r["factors"]["f_pred"] == 0.75 and r["weight"] == pytest.approx(0.615)
    cfg.chat.score_weights = {"cpu": 1, "runq": 0, "mem": 0, "net": 0}
    assert "warning" in await t.call("explain_score", {"node": "w1"})


async def test_task_stats(cfg):
    tasks = [{"node_id": "w1", "state": "done", "exec_ms": float(i), "t_alloc": NOW - 1000} for i in range(1, 101)]
    tasks += [{"node_id": "w2", "state": "timeout", "exec_ms": None, "t_alloc": NOW - 1000}]
    t, _, _ = mk(cfg, tasks=tasks)
    r = await t.call("get_task_stats", {"hours": 1})
    assert r["tasks"] == 101 and r["timeouts"] == 1 and r["p50_ms"] == 50 and r["p99_ms"] == 99 and r["max_ms"] == 100
    assert r["by_node"]["w2"]["timeouts"] == 1
    r = await t.call("get_task_stats", {"node": "w1"})
    assert r["tasks"] == 100 and "by_node" not in r
    assert "未知节点" in (await t.call("get_task_stats", {"node": "zz"}))["error"]


async def test_diagnoses_and_mysql_down(cfg):
    diags = [{"id": 7, "ts": NOW - 1000, "node": "w1", "severity": "high", "summary": "CPU 满", "label": "CPU 忙碌度",
              "report": {"causes": [{"cause": "stress 进程"}]}}]
    t, _, m = mk(cfg, diags=diags)
    r = await t.call("get_diagnoses", {})
    assert r["count"] == 1 and r["diagnoses"][0]["top_cause"] == "stress 进程"
    m.down = True
    assert "ConnectionError" in (await t.call("get_task_stats", {}))["error"]


def test_helpers():
    assert quantile([1, 2, 3, 4], 0.5) == 2 and quantile([], 0.5) is None
    assert bucketize([(0, 1), (500, 3), (1000, 5)], 1000) == [(0, 2), (1000, 5)]
    big = {"series": {"x": {"points": [[i, i] for i in range(2000)]}}}
    text = truncate_result(big, 2000)
    assert len(text.encode()) <= 2100 and "truncated" in text
    assert result_summary("list_nodes", {"count": 3, "online": 2}) == "3 台节点，在线 2"
    assert result_summary("x", {"error": "e"}) == "错误：e"
    json.loads(truncate_result({"a": 1}, 100))
