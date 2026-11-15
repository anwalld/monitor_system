import random

import numpy as np

from lmai.detector import Detector, sample_values
from lmai.windows import robust_z

from conftest import sample


def run(det, xs, start=0, node="w1", **kw):
    """逐秒喂样本，返回 [(秒序号, anomaly)]。xs 是 cpu 序列或 dict 序列。"""
    out = []
    for i, x in enumerate(xs):
        t = (start + i) * 1000
        args = dict(kw)
        if isinstance(x, dict):
            args.update(x)
        else:
            args["cpu"] = x
        for a in det.on_sample(sample(node=node, ts=t, **args), now_ms=t):
            out.append((start + i, a))
    return out


def noisy(n, base=0.03, amp=0.02, seed=1):
    rnd = random.Random(seed)
    return [base + rnd.uniform(-amp, amp) for _ in range(n)]


def test_robust_z_basic():
    w = np.array([1.0, 2, 3, 4, 5])
    r = robust_z(w, 10, 0.0)
    assert r.median == 3 and abs(r.scale - 1.4826) < 1e-9 and abs(r.z - 7 / 1.4826) < 1e-9
    assert robust_z(w, 0, 0.0, "down").z > 0   # 往下偏离对 down 指标是变坏
    assert robust_z(w, 10, 5.0).scale == 5.0   # MAD 下限


def test_sustained_cpu_detected_on_third_point(cfg):
    det = Detector(cfg.detect)
    assert run(det, noisy(300)) == []
    hits = run(det, [0.98] * 10, start=300)
    assert [(i, a.metric, a.rule) for i, a in hits] == [(302, "cpu", "zscore")]
    a = hits[0][1]
    assert a.severity == "high" and a.z > 10 and abs(a.median - 0.03) < 0.02


def test_spike_not_detected(cfg):
    det = Detector(cfg.detect)
    xs = noisy(300) + [0.99, 0.99] + noisy(20, seed=2) + [0.99] + noisy(20, seed=3)
    assert run(det, xs) == []


def test_constant_zero_metric_needs_real_change(cfg):
    det = Detector(cfg.detect)
    run(det, [0.03] * 300)
    # 超时率一直为 0：0.1（2 个尺度）不报
    assert run(det, [{"nics": [{"name": "eth0", "rtt_ms": 1, "timeout_rate": 0.1}]}] * 5, start=300) == []
    hits = run(det, [{"nics": [{"name": "eth0", "rtt_ms": 1, "timeout_rate": 0.5}]}] * 5, start=305)
    metrics = {(a.rule, a.metric) for _, a in hits}
    assert ("zscore", "nic.eth0.timeout_rate") in metrics


def test_needs_min_samples(cfg):
    det = Detector(cfg.detect)
    assert run(det, noisy(30) + [0.98] * 10) == []


def test_direction_down_only(cfg):
    det = Detector(cfg.detect)
    run(det, [{"cpu": 0.5, "mem": 0.8}] * 300)
    # CPU 骤降、内存上升都不是变坏
    assert run(det, [{"cpu": 0.0, "mem": 0.95}] * 5, start=300) == []
    hits = run(det, [{"cpu": 0.5, "mem": 0.3}] * 5, start=305)
    assert [a.metric for _, a in hits] == ["mem_avail"]


def test_reported_once_per_episode(cfg):
    det = Detector(cfg.detect)
    run(det, noisy(300))
    hits = run(det, [0.98] * 60, start=300)
    assert len(hits) == 1


def test_threshold_rules_use_10s_average(cfg):
    det = Detector(cfg.detect)
    run(det, noisy(300))
    # 单秒超时率在 0 和 0.5 之间跳（平均 0.25 > 0.2）→ 阈值规则报；每秒取值不连续越界时 z 规则不会每秒都报
    nic = lambda r: {"nics": [{"name": "eth0", "rtt_ms": 1, "timeout_rate": r}]}
    xs = [nic(0.5 if i % 2 else 0.0) for i in range(30)]
    hits = [(i, a) for i, a in run(det, xs, start=300) if a.rule == "threshold"]
    assert len(hits) == 1 and hits[0][1].metric == "nic.eth0.timeout_rate"
    # 11% 的基线丢包（虚拟机 8.8.8.8 的情况）不报
    det2 = Detector(cfg.detect)
    xs = [nic(0.5 if i % 9 == 0 else 0.0) for i in range(400)]
    assert [a for _, a in run(det2, xs) if a.rule == "threshold"] == []


def test_runq_drop_rate_threshold(cfg):
    det = Detector(cfg.detect)
    run(det, noisy(100))
    hits = run(det, [{"cpu": 0.03, "drop": 0.05}] * 20, start=100)
    assert any(a.rule == "threshold" and a.metric == "runq_drop_rate" for _, a in hits)


def test_events(cfg):
    det = Detector(cfg.detect)
    now = 1_000_000
    a = det.on_event("w1", "evict", now - 1000, {"reason": "probe"}, now)
    assert len(a) == 1 and a[0].severity == "high" and "probe" in a[0].describe()
    assert det.on_event("w1", "slow_start", now, {}, now) == []
    assert det.on_event("w1", "evict", now - 700_000, {}, now) == []  # 太旧


def test_dedup_and_escalation(cfg):
    det = Detector(cfg.detect)
    from lmai.detector import Anomaly
    mk = lambda sev, t: Anomaly(node="w1", ts_ms=t, rule="zscore", metric="cpu", severity=sev)
    assert det.admit(mk("medium", 0), 0)
    a2 = mk("medium", 60_000)
    assert not det.admit(a2, 60_000) and a2.deduped
    assert det.admit(mk("high", 120_000), 120_000)            # 升级
    assert not det.admit(mk("high", 300_000), 300_000)
    assert det.admit(mk("medium", 720_001), 720_001)          # 距上次诊断超过 10 分钟
    other = Anomaly(node="w2", ts_ms=0, rule="event", metric="evict", severity="high")
    assert det.admit(other, 720_002)                          # 按节点去重


def test_warm_then_detect_immediately(cfg):
    det = Detector(cfg.detect)
    det.warm("w1", [sample(ts=i * 1000, cpu=c) for i, c in enumerate(noisy(300))])
    hits = run(det, [0.98] * 3, start=300)
    assert len(hits) == 1
    # 重复样本（预热与 Stream 重叠）被忽略
    assert det.on_sample(sample(ts=1000, cpu=0.99), 0) == []


def test_recent_metrics_and_peers(cfg):
    det = Detector(cfg.detect)
    run(det, [0.1] * 600, node="w1")
    run(det, [0.3] * 600, node="w2")
    run(det, [0.5] * 600, node="w3")
    rm = det.recent_metrics("w1", 10)
    assert len(rm["ts"]) == 60 and rm["cpu"][0] == 0.1 and rm["ts"][1] - rm["ts"][0] == 10000
    peer = det.peer_median("w1")
    assert peer["nodes"] == 2 and peer["cpu"] == 0.4 and peer["rtt_ms"] == 1.0


def test_sample_values_defaults():
    v = sample_values({"node_id": "x", "ts_ms": "5"})  # proto3 JSON 省略 0 值
    assert v == {"cpu": 0.0, "mem_avail": 0.0, "disk": 0.0}


def test_related_window(cfg):
    from lmai.detector import Anomaly
    det = Detector(cfg.detect)
    mk = lambda t, m, n="w1": Anomaly(node=n, ts_ms=t, rule="zscore", metric=m, severity="medium")
    xs = [mk(0, "a"), mk(50_000, "b"), mk(100_000, "trig"), mk(102_000, "evict"), mk(110_000, "late"), mk(101_000, "x", "w2")]
    for x in xs:
        det._record(x)
    trig = xs[2]
    assert [x.metric for x in det.related(trig)] == ["b"]
    assert [x.metric for x in det.related(trig, 5000)] == ["b", "evict"]


def test_related_dedup_by_metric_and_reporting(cfg):
    from lmai.detector import Anomaly
    det = Detector(cfg.detect)
    mk = lambda t, m, z: Anomaly(node="w1", ts_ms=t, rule="zscore", metric=m, severity="medium", z=z)
    trig = mk(100_000, "disk", 9)
    for x in [mk(90_000, "disk", 5), mk(95_000, "cpu", 5), mk(96_000, "cpu", 8), trig, mk(101_000, "disk", 12)]:
        det._record(x)
    rel = det.related(trig, 5000)
    assert [(x.metric, x.z) for x in rel] == [("cpu", 8)]   # 与触发同指标的不重复带上
    run(det, [0.03] * 100)                                  # 样本 ts 0..99 s
    rep = det.reporting("w1", 160_000)
    assert rep["last_sample_age_s"] == 61.0 and rep["samples_10m"] == 100 and rep["missing_s_10m"] == 60
