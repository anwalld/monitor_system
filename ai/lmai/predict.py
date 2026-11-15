"""负载预测（M9）：Holt 线性指数平滑预测 30 秒后的 CPU 忙碌度，换算成调整因子推给 Manager。

纯计算部分（Holt、因子、在线 MAE）只用标准库，`experiments/predict_eval.py` 离线回放时直接复用。
gRPC 推送在 Pusher 里，生成代码在 lmai/pb（镜像构建时由 grpc_tools.protoc 生成）。
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field
from typing import Any


def factor(y: float, knee: float = 0.7, fmin: float = 0.5) -> float:
    """预测值 → 调整因子：≤ knee 为 1，knee 到 1.0 线性降到 fmin，≥ 1.0 为 fmin。只降不升。"""
    if y <= knee:
        return 1.0
    if y >= 1.0:
        return fmin
    return 1.0 - (1.0 - fmin) * (y - knee) / (1.0 - knee)


def _damp_sum(phi: float, h: float) -> float:
    """Σ_{i=1..h} φ^i（h 可以是小数）；φ = 1 时为 h。"""
    if phi >= 1.0:
        return h
    return phi * (1 - phi ** h) / (1 - phi)


@dataclass
class Holt:
    """Holt 线性指数平滑（水平 + 趋势），可选阻尼趋势（Gardner–McKenzie）：φ < 1 时趋势每秒乘 φ，
    外推 h 秒只加 Σφ^i 倍的趋势，避免把一时的上升/下降线性外推 30 秒。φ = 1 即普通 Holt。
    趋势按“每秒”计，样本间隔不是 1 秒时按实际秒数折算。"""
    alpha: float = 0.3
    beta: float = 0.1
    phi: float = 1.0
    level: float = 0.0
    trend: float = 0.0
    n: int = 0
    last_ts: int = -1

    def update(self, ts_ms: int, x: float) -> None:
        if self.n == 0:
            self.level, self.trend = x, 0.0
        else:
            dt = (ts_ms - self.last_ts) / 1000.0
            if dt <= 0:
                return
            prior = self.level + _damp_sum(self.phi, dt) * self.trend
            level = self.alpha * x + (1 - self.alpha) * prior
            self.trend = (self.beta * (level - self.level) / dt
                          + (1 - self.beta) * (self.phi ** dt) * self.trend)
            self.level = level
        self.n += 1
        self.last_ts = ts_ms

    def forecast(self, horizon_s: float) -> float:
        return min(1.0, max(0.0, self.level + _damp_sum(self.phi, horizon_s) * self.trend))


@dataclass
class _Pending:
    target_ts: int
    holt: float
    naive: float


@dataclass
class _Node:
    holt: Holt
    last_x: float = 0.0
    last_ts: int = -1
    pending: deque = field(default_factory=deque)          # 待对答案的预测
    errs: deque = field(default_factory=deque)             # (ts, |holt − 实际|, |naive − 实际|)
    latest: dict[str, Any] | None = None                    # 最近一次输出的预测


class Predictor:
    def __init__(self, horizon_s: float = 30, alpha: float = 0.3, beta: float = 0.05, min_samples: int = 30,
                 fresh_s: float = 10, knee: float = 0.7, fmin: float = 0.5, eval_window_s: float = 3600,
                 phi: float = 0.9):
        self.horizon_s = horizon_s
        self.alpha, self.beta, self.phi = alpha, beta, phi
        self.min_samples = min_samples
        self.fresh_ms = fresh_s * 1000
        self.knee, self.fmin = knee, fmin
        self.window_ms = eval_window_s * 1000
        self.nodes: dict[str, _Node] = {}
        self.ticks = 0

    @classmethod
    def from_cfg(cls, c: Any) -> "Predictor":
        return cls(c.horizon_s, c.alpha, c.beta, c.min_samples, c.fresh_s, c.factor_knee, c.factor_min,
                   c.eval_window_s, c.phi)

    def observe(self, node: str, ts_ms: int, x: float) -> None:
        if not node or not math.isfinite(x):
            return
        st = self.nodes.get(node)
        if st is None:
            st = self.nodes[node] = _Node(Holt(self.alpha, self.beta, self.phi))
        if ts_ms <= st.last_ts:
            return
        # 先对答案：目标时刻已到的预测，用第一条不早于目标时刻的样本作为实际值
        while st.pending and st.pending[0].target_ts <= ts_ms:
            p = st.pending.popleft()
            st.errs.append((ts_ms, abs(p.holt - x), abs(p.naive - x)))
        while st.errs and st.errs[0][0] < ts_ms - self.window_ms:
            st.errs.popleft()
        st.holt.update(ts_ms, x)
        st.last_x, st.last_ts = x, ts_ms

    def tick(self, now_ms: int) -> list[dict[str, Any]]:
        """为新鲜（最后样本不超过 fresh_s）且样本足够的节点输出预测，并记下待对答案的记录。"""
        self.ticks += 1
        out = []
        for node, st in self.nodes.items():
            if st.holt.n < self.min_samples or now_ms - st.last_ts > self.fresh_ms:
                continue
            y = st.holt.forecast(self.horizon_s)
            f = factor(y, self.knee, self.fmin)
            target = st.last_ts + int(self.horizon_s * 1000)
            if not st.pending or st.pending[-1].target_ts < target:
                st.pending.append(_Pending(target, y, st.last_x))
            st.latest = {"cpu_busy_30s": round(y, 4), "factor": round(f, 4), "cpu_now": round(st.last_x, 4),
                         "ts_ms": now_ms}
            out.append({"node_id": node, **st.latest})
        return out

    @staticmethod
    def _mae(errs: Any) -> dict[str, Any]:
        n = len(errs)
        if n == 0:
            return {"points": 0, "mae_holt": None, "mae_naive": None}
        return {"points": n, "mae_holt": round(sum(e[1] for e in errs) / n, 4),
                "mae_naive": round(sum(e[2] for e in errs) / n, 4)}

    def view(self, now_ms: int) -> dict[str, Any]:
        nodes = {}
        all_errs: list[tuple[int, float, float]] = []
        for node, st in sorted(self.nodes.items()):
            errs = [e for e in st.errs if e[0] >= now_ms - self.window_ms]
            all_errs.extend(errs)
            nodes[node] = {**(st.latest or {}), "fresh": now_ms - st.last_ts <= self.fresh_ms,
                           "samples": st.holt.n, **self._mae(errs)}
        return {"horizon_s": self.horizon_s, "alpha": self.alpha, "beta": self.beta, "phi": self.phi,
                "window_s": self.window_ms / 1000, "nodes": nodes,
                "total": self._mae(all_errs)}


class Pusher:
    """Advisor.PushPredictions 客户端（grpc.aio，懒连接）。"""

    def __init__(self, addr: str, timeout_s: float = 2):
        self.addr = addr
        self.timeout_s = timeout_s
        self._stub: Any = None
        self._channel: Any = None

    def _ensure(self) -> Any:
        if self._stub is None:
            import grpc  # noqa: PLC0415  延迟导入：纯计算部分不依赖 grpc

            from .pb import advisor_pb2_grpc  # noqa: PLC0415
            self._channel = grpc.aio.insecure_channel(self.addr)
            self._stub = advisor_pb2_grpc.AdvisorStub(self._channel)
        return self._stub

    async def push(self, items: list[dict[str, Any]], ts_ms: int, ttl_ms: int) -> None:
        from .pb import advisor_pb2  # noqa: PLC0415
        stub = self._ensure()
        req = advisor_pb2.PredictionBatch(ts_ms=ts_ms, ttl_ms=ttl_ms, items=[
            advisor_pb2.Prediction(node_id=i["node_id"], cpu_busy_30s=i["cpu_busy_30s"], factor=i["factor"])
            for i in items])
        await stub.PushPredictions(req, timeout=self.timeout_s)

    async def close(self) -> None:
        if self._channel is not None:
            await self._channel.close()
            self._channel = self._stub = None
