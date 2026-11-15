"""按时间保留的滑动窗口与稳健 z 分数。"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass

import numpy as np

MAD_K = 1.4826  # 正态分布下 MAD → 标准差的换算系数


class Series:
    """一个指标最近 window_ms 毫秒的 (ts_ms, value)，按 ts 递增追加。"""

    __slots__ = ("window_ms", "ts", "vals")

    def __init__(self, window_ms: float):
        self.window_ms = window_ms
        self.ts: deque[int] = deque()
        self.vals: deque[float] = deque()

    def __len__(self) -> int:
        return len(self.vals)

    def add(self, ts_ms: int, value: float) -> None:
        self.ts.append(ts_ms)
        self.vals.append(value)
        self.trim(ts_ms)

    def trim(self, now_ms: int) -> None:
        cutoff = now_ms - self.window_ms
        while self.ts and self.ts[0] < cutoff:
            self.ts.popleft()
            self.vals.popleft()

    def last(self) -> float | None:
        return self.vals[-1] if self.vals else None

    def mean_since(self, since_ms: int) -> float | None:
        s, n = 0.0, 0
        for t, v in zip(reversed(self.ts), reversed(self.vals)):
            if t < since_ms:
                break
            s += v
            n += 1
        return s / n if n else None

    def array(self) -> np.ndarray:
        return np.fromiter(self.vals, dtype=np.float64, count=len(self.vals))


@dataclass
class ZResult:
    z: float        # 正数表示往“变坏”方向偏离
    median: float
    scale: float    # max(1.4826 × MAD, min_scale)


def robust_z(window: np.ndarray, x: float, min_scale: float, direction: str = "up") -> ZResult:
    med = float(np.median(window))
    mad = float(np.median(np.abs(window - med)))
    scale = max(MAD_K * mad, min_scale)
    dev = (x - med) if direction == "up" else (med - x)
    return ZResult(dev / scale, med, scale)
