"""诊断上下文组装、系统提示词、节点名匿名化。"""

from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from typing import Any

from .detector import Anomaly
from .schema import SCHEMA_TEXT

# node:{id} Hash 里给模型看的字段（数字和状态，不含地址）
NODE_FIELDS = (
    "state", "state_reason", "evicted", "evict_reason", "score", "s_cpu", "s_runq", "s_mem", "s_net",
    "prob", "f_slow", "f_inflight", "f_timeout", "timeout_count", "inflight_tasks", "unconfirmed",
    "probe_fail_streak", "ncpu", "cpu_busy", "mem_avail_ratio", "disk_util", "runq_p99_us",
    "runq_drop_rate", "best_nic",
)

SYSTEM_PROMPT = f"""你是 Linux 服务器集群的运维诊断助手。调度系统把任务派给最空闲的节点，统计检测器在某台节点上发现了异常，请根据给出的上下文判断可能的原因。

指标含义（括号里是调度系统的评分区间：好 → 差，以及剔除线）：
- cpu：全机 CPU 忙碌度 0~1（0.2 → 0.9，剔除 0.97）。
- runq_p99_us：调度延迟（进程就绪到真正上 CPU 的等待）P99，微秒（100 → 2000，剔除 20000）。CPU 被挤满、核数不够时升高。
- runq_drop_rate：调度延迟采集的丢弃率，超过 1% 说明内核事件过多或采集跟不上。
- mem_avail：可用内存比例 0~1（0.5 → 0.1，剔除 0.05），越小越差。
- disk：磁盘利用率 0~1（io_ticks 换算）。
- nic.<网卡>.rtt_ms：ICMP 探测 RTT 毫秒（5 → 100）；timeout_rate：探测超时率 0~1（0 → 0.1，所有网卡 ≥ 0.5 时剔除）；retrans_rate：TCP 重传率（0 → 0.05）。
- 节点状态 state：normal 正常、slow_start 慢启动（刚登记或刚恢复，权重从 0.2 升到 1）、evicted 剔除、rescue 急救（超时计数高时用 1 ms 级探测确认节点是否还活着）。
- f_timeout = exp(−0.5 × T)，T 是半衰期 10 分钟的任务超时计数；prob 是选中概率。
- 异常规则：zscore 表示与该节点最近 10 分钟中位数相比的稳健 z 分数（z = (x − 中位数) / (1.4826 × MAD)）连续 3 秒超过 4；threshold 表示固定阈值；event 表示调度器事件（evict 剔除、rescue_start 开始急救）。
- peer_median 是同一时刻其他节点的中位数，用来区分单台节点的问题和全局问题。
- reporting：last_sample_age_s 是最后一条样本距诊断时刻的秒数，missing_s_10m 是最近 10 分钟缺失的秒数。Worker 每秒上报一次，样本中断说明 Worker 进程停止、卡住或到 Manager 的网络断开；evict 的 reason=lost 表示上报中断，reason=probe 表示 Manager 探测 Worker 失败。注意区分“节点不再上报”和“某块网卡指标变差”：早先网卡指标异常不一定是这次剔除的原因，要看时间先后。
- 时间都是服务器本地时间，引用时保留原格式。

要求：
1. 只根据上下文给出的数据推断，结论和每条 evidence 都要引用上下文里的具体数值（带指标名）。
2. 原因按可能性从高到低排列，confidence 在 0~1 之间；数据不足以判断时如实说明并给较低的置信度。
3. actions 是给运维人员的建议（例如登录节点查看哪些进程、检查哪块网卡），系统不会自动执行。
4. 用中文，只输出一个 JSON 对象，不要输出其他文字。格式：
{SCHEMA_TEXT}"""


def iso(ms: int | None) -> str | None:
    if ms is None:
        return None
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).astimezone().isoformat(timespec="seconds")


def node_state(h: dict[str, str]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for k in NODE_FIELDS:
        if k in h:
            v = h[k]
            try:
                out[k] = float(v) if "." in v else int(v)
            except ValueError:
                out[k] = v
    return out


def anomaly_view(a: Anomaly) -> dict[str, Any]:
    d = a.to_dict()
    d["ts"] = iso(a.ts_ms)
    for k in ("ts_ms", "sample_ts_ms", "deduped"):
        d.pop(k, None)
    return {k: v for k, v in d.items() if v not in (None, {}, [])}


def recent_view(rm: dict[str, Any]) -> dict[str, Any]:
    out = dict(rm)
    ts = out.pop("ts", [])
    out["start"] = iso(ts[0]) if ts else None
    out["points"] = len(ts)
    return out


def build_context(anomaly: Anomaly, related: list[Anomaly], node_hash: dict[str, str],
                  recent: dict[str, Any], transitions: list[dict[str, Any]],
                  task_timeouts: dict[str, Any] | None, peer: dict[str, Any],
                  reporting: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "anomaly": anomaly_view(anomaly),
        "related_anomalies": [anomaly_view(x) for x in related],
        "node_state": node_state(node_hash),
        "reporting": reporting or {},
        "recent_metrics": recent_view(recent),
        "state_transitions": [
            {"ts": iso(t.get("ts_ms")), "kind": t.get("kind"), "detail": t.get("detail")}
            for t in transitions
        ],
        "task_timeouts": task_timeouts,
        "peer_median": peer,
    }


class Anonymizer:
    """节点名 ↔ 代号（node-1、node-2……按字典序）。单次正则替换，不会把替换结果再替换一遍。"""

    def __init__(self, names: list[str], enabled: bool):
        self.enabled = enabled and bool(names)
        uniq = sorted(set(n for n in names if n))
        self.fwd = {n: f"node-{i + 1}" for i, n in enumerate(uniq)}
        self.rev = {v: k for k, v in self.fwd.items()}

    @staticmethod
    def _sub(text: str, table: dict[str, str]) -> str:
        if not table:
            return text
        keys = sorted(table, key=len, reverse=True)
        pat = re.compile(r"(?<![\w-])(" + "|".join(map(re.escape, keys)) + r")(?![\w-])")
        return pat.sub(lambda m: table[m.group(1)], text)

    def hide(self, obj: Any) -> str:
        text = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
        return self._sub(text, self.fwd) if self.enabled else text

    def reveal(self, obj: Any) -> Any:
        if not self.enabled:
            return obj
        if isinstance(obj, str):
            return self._sub(obj, self.rev)
        if isinstance(obj, list):
            return [self.reveal(x) for x in obj]
        if isinstance(obj, dict):
            return {k: self.reveal(v) for k, v in obj.items()}
        return obj
