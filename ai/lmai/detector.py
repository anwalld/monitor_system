"""异常检测：稳健 z 分数、阈值规则、事件规则、去重。纯计算，不做 IO。"""

from __future__ import annotations

import math
import statistics
from collections import deque
from dataclasses import asdict, dataclass, field
from typing import Any

from .config import DetectCfg
from .windows import Series, robust_z

SEV_RANK = {"low": 0, "medium": 1, "high": 2}

METRIC_LABEL = {
    "cpu": "CPU 忙碌度",
    "runq_p99_us": "调度延迟 P99（µs）",
    "mem_avail": "可用内存比例",
    "disk": "磁盘利用率",
    "rtt_ms": "RTT（ms）",
    "timeout_rate": "探测超时率",
    "retrans_rate": "TCP 重传率",
    "runq_drop_rate": "调度延迟丢弃率",
}


def base_metric(key: str) -> str:
    """nic.ens160.rtt_ms → rtt_ms。"""
    return key.rsplit(".", 1)[-1]


def metric_label(key: str) -> str:
    parts = key.split(".")
    if len(parts) == 3 and parts[0] == "nic":
        return f"{parts[1]} {METRIC_LABEL.get(parts[2], parts[2])}"
    return METRIC_LABEL.get(key, key)


def _f(v: Any, default: float = 0.0) -> float:
    try:
        x = float(v)
    except (TypeError, ValueError):
        return default
    return x if math.isfinite(x) else default


def sample_values(s: dict[str, Any]) -> dict[str, float]:
    """proto JSON 样本 → 指标字典。proto3 JSON 省略取值为 0 的普通字段，缺失按 0 处理；
    runq 是 optional 字段，缺失表示没上报。"""
    out = {
        "cpu": _f(s.get("cpu_busy")),
        "mem_avail": _f(s.get("mem_avail_ratio")),
        "disk": _f(s.get("disk_util")),
    }
    if "runq_p99_us" in s:
        out["runq_p99_us"] = _f(s.get("runq_p99_us"))
        out["runq_drop_rate"] = _f(s.get("runq_drop_rate"))
    for n in s.get("nics") or []:
        name = n.get("name") or "?"
        for k in ("rtt_ms", "timeout_rate", "retrans_rate"):
            out[f"nic.{name}.{k}"] = _f(n.get(k))
    return out


@dataclass
class Anomaly:
    node: str
    ts_ms: int                 # 检出时刻（AI 服务墙上时间）
    rule: str                  # zscore / threshold / event
    metric: str                # 指标 key，或事件种类
    severity: str              # 检测器给的严重度
    value: float | None = None
    median: float | None = None
    scale: float | None = None
    z: float | None = None
    sample_ts_ms: int | None = None
    detail: dict[str, Any] = field(default_factory=dict)
    deduped: bool = False

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        for k in ("value", "median", "scale", "z"):
            if d[k] is not None:
                d[k] = round(d[k], 4)
        d["label"] = metric_label(self.metric) if self.rule != "event" else self.metric
        return d

    def describe(self) -> str:
        """代码生成的一句话（不调用 LLM 时作为诊断结论）。"""
        if self.rule == "event":
            if self.metric == "evict":
                return f"节点被剔除，原因 {self.detail.get('reason', '?')}"
            if self.metric == "rescue_start":
                return f"节点进入急救，超时计数 T {self.detail.get('timeout_count', '?')}"
            return f"事件 {self.metric}"
        label = metric_label(self.metric)
        if self.rule == "threshold":
            return f"{label} 最近 10 秒平均 {self.value:.4g}，超过阈值 {self.detail.get('limit')}"
        word = "低" if self.detail.get("direction") == "down" else "高"
        return (f"{label} {self.value:.4g}，比最近 10 分钟中位数 {self.median:.4g} "
                f"{word} {abs(self.z):.1f} 个尺度")


class _NodeState:
    def __init__(self) -> None:
        self.series: dict[str, Series] = {}
        self.streak: dict[str, int] = {}
        self.last_ts: int = -1
        self.last_values: dict[str, float] = {}
        self.last_diag_ms: int | None = None
        self.last_diag_sev: str | None = None


class Detector:
    def __init__(self, cfg: DetectCfg):
        self.cfg = cfg
        self.window_ms = cfg.window_s * 1000
        self.nodes: dict[str, _NodeState] = {}
        self.recent: deque[Anomaly] = deque(maxlen=1000)
        self.total = 0

    # ---------- 样本 ----------
    def _node(self, node: str) -> _NodeState:
        st = self.nodes.get(node)
        if st is None:
            st = self.nodes[node] = _NodeState()
        return st

    def _series(self, st: _NodeState, key: str) -> Series:
        s = st.series.get(key)
        if s is None:
            s = st.series[key] = Series(self.window_ms)
        return s

    def warm(self, node: str, samples: list[dict[str, Any]]) -> int:
        """用历史样本填窗口，不检测。返回填入条数。"""
        n = 0
        for s in sorted(samples, key=lambda x: int(_f(x.get("ts_ms")))):
            n += self._ingest(node, s, detect=False, now_ms=0) is not None
        return n

    def on_sample(self, s: dict[str, Any], now_ms: int) -> list[Anomaly]:
        node = s.get("node_id")
        if not node:
            return []
        return self._ingest(node, s, detect=True, now_ms=now_ms) or []

    def _ingest(self, node: str, s: dict[str, Any], detect: bool, now_ms: int) -> list[Anomaly] | None:
        st = self._node(node)
        ts = int(_f(s.get("ts_ms")))
        if ts <= st.last_ts:  # 重复或乱序
            return None
        st.last_ts = ts
        vals = sample_values(s)
        st.last_values = vals
        out: list[Anomaly] = []
        cfg = self.cfg
        for key, x in vals.items():
            ser = self._series(st, key)
            mc = cfg.metrics.get(base_metric(key))
            if detect and mc is not None:
                exceeded, zr = False, None
                if len(ser) >= cfg.min_samples:
                    zr = robust_z(ser.array(), x, mc.min_scale, mc.direction)
                    exceeded = zr.z > cfg.z_threshold
                a = self._streak(st, "z:" + key, exceeded)
                if a and zr is not None:
                    out.append(Anomaly(
                        node=node, ts_ms=now_ms, rule="zscore", metric=key,
                        severity="high" if zr.z >= cfg.z_high else "medium",
                        value=x, median=zr.median, scale=zr.scale, z=zr.z, sample_ts_ms=ts,
                        detail={"direction": mc.direction, "window": len(ser)}))
            ser.add(ts, x)
        for s_ in st.series.values():
            s_.trim(ts)
        if detect:
            out.extend(self._thresholds(node, st, ts, now_ms))
        for a in out:
            self._record(a)
        return out

    def _streak(self, st: _NodeState, key: str, exceeded: bool) -> bool:
        """连续越界计数；恰好达到 consecutive 时返回 True（一段持续越界只报一次）。"""
        if not exceeded:
            st.streak[key] = 0
            return False
        n = st.streak.get(key, 0) + 1
        st.streak[key] = n
        return n == self.cfg.consecutive

    def _thresholds(self, node: str, st: _NodeState, ts: int, now_ms: int) -> list[Anomaly]:
        cfg = self.cfg
        since = ts - int(cfg.threshold_avg_s * 1000) + 1
        out = []
        checks = []
        if "runq_drop_rate" in st.last_values:
            checks.append(("runq_drop_rate", cfg.runq_drop_rate_max))
        checks += [(k, cfg.nic_timeout_max) for k in st.last_values if k.endswith(".timeout_rate")]
        for key, limit in checks:
            avg = st.series[key].mean_since(since)
            exceeded = avg is not None and avg > limit
            if self._streak(st, "t:" + key, exceeded):
                out.append(Anomaly(node=node, ts_ms=now_ms, rule="threshold", metric=key,
                                   severity="medium", value=avg, sample_ts_ms=ts,
                                   detail={"limit": limit, "avg_s": cfg.threshold_avg_s}))
        # 不再上报的指标（例如网卡消失）清掉计数
        for k in [k for k in st.streak if k.startswith("t:") and k[2:] not in st.last_values]:
            del st.streak[k]
        return out

    # ---------- 事件 ----------
    def on_event(self, node: str, kind: str, ts_ms: int, detail: dict[str, Any], now_ms: int) -> list[Anomaly]:
        if kind not in self.cfg.event_kinds or not node:
            return []
        if now_ms - ts_ms > self.cfg.event_max_age_s * 1000:
            return []
        a = Anomaly(node=node, ts_ms=now_ms, rule="event", metric=kind, severity="high",
                    sample_ts_ms=ts_ms, detail=dict(detail or {}))
        self._record(a)
        return [a]

    def _record(self, a: Anomaly) -> None:
        self.total += 1
        self.recent.append(a)

    # ---------- 去重 ----------
    def admit(self, a: Anomaly, now_ms: int) -> bool:
        """是否为这个异常发起诊断。允许时记下时间与严重度；不允许时标记 deduped。"""
        st = self._node(a.node)
        if (st.last_diag_ms is not None and now_ms - st.last_diag_ms < self.cfg.dedup_s * 1000
                and SEV_RANK[a.severity] <= SEV_RANK[st.last_diag_sev or "low"]):
            a.deduped = True
            return False
        st.last_diag_ms, st.last_diag_sev = now_ms, a.severity
        return True

    def related(self, a: Anomaly, after_ms: int = 0) -> list[Anomaly]:
        """同一节点在 a 之前 related_s 秒到之后 after_ms 毫秒内的其他异常。"""
        lo, hi = a.ts_ms - self.cfg.related_s * 1000, a.ts_ms + after_ms
        best: dict[tuple[str, str], Anomaly] = {}  # 同一指标反复越界只保留偏离最大的一次
        for x in self.recent:
            if x is a or x.node != a.node or not lo <= x.ts_ms <= hi or (x.rule, x.metric) == (a.rule, a.metric):
                continue
            k = (x.rule, x.metric)
            if k not in best or abs(x.z or 0) > abs(best[k].z or 0):
                best[k] = x
        return sorted(best.values(), key=lambda x: x.ts_ms)

    def reporting(self, node: str, now_ms: int) -> dict[str, Any]:
        """上报情况：最后一条样本距今多久、最近 10 分钟缺了多少秒（节点停止上报时 LLM 要知道）。"""
        st = self.nodes.get(node)
        if st is None or st.last_ts < 0:
            return {"samples_10m": 0}
        ser = st.series.get("cpu")
        n = len(ser) if ser else 0
        span = min(self.cfg.window_s, max(1.0, (now_ms - ser.ts[0]) / 1000)) if n else self.cfg.window_s
        return {"last_sample_age_s": round(max(0, now_ms - st.last_ts) / 1000, 1),
                "samples_10m": n, "missing_s_10m": max(0, int(span) - n)}

    # ---------- 给诊断用的视图 ----------
    def recent_metrics(self, node: str, step_s: int, minutes: float = 10) -> dict[str, Any]:
        st = self.nodes.get(node)
        if st is None or st.last_ts < 0:
            return {"step_s": step_s, "ts": []}
        step = step_s * 1000
        lo = st.last_ts - int(minutes * 60000)
        buckets: dict[str, dict[int, list[float]]] = {}
        all_b: set[int] = set()
        for key, ser in st.series.items():
            per: dict[int, list[float]] = {}
            for t, v in zip(ser.ts, ser.vals):
                if t < lo:
                    continue
                b = t - t % step
                per.setdefault(b, []).append(v)
                all_b.add(b)
            buckets[key] = per
        order = sorted(all_b)
        out: dict[str, Any] = {"step_s": step_s, "ts": order}
        for key, per in buckets.items():
            agg = max if base_metric(key) == "runq_p99_us" else (lambda xs: sum(xs) / len(xs))
            out[key] = [round(agg(per[b]), 4) if b in per else None for b in order]
        return out

    @staticmethod
    def summarize(vals: dict[str, float]) -> dict[str, float]:
        """节点一条样本的概括：网卡三项取各网卡平均。"""
        out = {k: v for k, v in vals.items() if not k.startswith("nic.")}
        for m in ("rtt_ms", "timeout_rate", "retrans_rate"):
            xs = [v for k, v in vals.items() if k.startswith("nic.") and k.endswith("." + m)]
            if xs:
                out[m] = sum(xs) / len(xs)
        return out

    def peer_median(self, node: str, max_age_ms: int = 5000) -> dict[str, Any]:
        newest = max((st.last_ts for st in self.nodes.values()), default=-1)
        peers = [self.summarize(st.last_values) for n, st in self.nodes.items()
                 if n != node and st.last_values and newest - st.last_ts <= max_age_ms]
        out: dict[str, Any] = {"nodes": len(peers)}
        keys = sorted({k for p in peers for k in p})
        for k in keys:
            xs = [p[k] for p in peers if k in p]
            out[k] = round(statistics.median(xs), 4)
        return out

    def node_count(self) -> int:
        return len(self.nodes)
