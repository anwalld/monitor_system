"""运维问答的只读工具（M9）。同一份实现给看板问答（OpenAI 函数调用）和 MCP server 用。

每个工具返回 dict；参数不对、未知节点、数据源不可用时返回带 "error" 的 dict，不抛异常。
时间一律输出服务器本地时间。
"""

from __future__ import annotations

import json
import logging
import math
import time
from datetime import datetime
from typing import Any, Awaitable, Callable

from .config import Config
from .store import MySQL, Redis

log = logging.getLogger("lmai.tools")

# 每秒样本里能查的指标（网卡类按网卡分列）
NIC_METRICS = ("rtt_ms", "timeout_rate", "retrans_rate", "rx_bps", "tx_bps")
NODE_METRICS = ("cpu", "mem_avail", "disk", "runq_p99_us")
ALL_METRICS = NODE_METRICS + NIC_METRICS
# 5 分钟聚合表里对应的列（disk、流量没有聚合）
MYSQL_COLUMNS = {
    "cpu": "cpu_avg", "mem_avail": "mem_avail_min", "runq_p99_us": "runq_p99_max_us",
    "rtt_ms": "net_rtt_avg_ms", "timeout_rate": "net_timeout_rate", "retrans_rate": "net_retrans_rate",
}
MAX_POINTS = 60

SPECS: list[dict[str, Any]] = [
    {
        "name": "list_nodes",
        "description": "列出所有节点的当前状态（normal 正常 / slow_start 慢启动 / evicted 剔除 / rescue 急救）、"
                       "总分 score、选中概率 prob、最新 CPU/内存/调度延迟、二次调整因子和负载预测。"
                       "回答“哪台最闲/最忙、有几台在线、谁被剔除”等问题时先调用它。",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "get_node_metrics",
        "description": "取一台节点某个指标最近一段时间的序列（自动降采样到不超过 60 个点），返回每个点和 min/avg/max。"
                       "minutes ≤ 60 用每秒样本；更长（≤ 1440）用 5 分钟聚合（没有 disk、rx_bps、tx_bps）。"
                       "metric：cpu（CPU 忙碌度 0~1）、mem_avail（可用内存比例 0~1）、disk（磁盘利用率 0~1）、"
                       "runq_p99_us（调度延迟 P99，微秒）、rtt_ms、timeout_rate、retrans_rate、rx_bps、tx_bps（按网卡分列）。",
        "parameters": {
            "type": "object",
            "properties": {
                "node": {"type": "string", "description": "节点 id"},
                "metric": {"type": "string", "enum": list(ALL_METRICS)},
                "minutes": {"type": "integer", "minimum": 1, "maximum": 1440, "default": 10},
            },
            "required": ["node", "metric"], "additionalProperties": False,
        },
    },
    {
        "name": "get_node_events",
        "description": "一台节点最近的状态转移与调度事件（evict 剔除及原因、rescue_start/rescue_end 急救、"
                       "slow_start 慢启动、normal 恢复正常），新的在前，最多 100 条。",
        "parameters": {
            "type": "object",
            "properties": {
                "node": {"type": "string"},
                "hours": {"type": "number", "minimum": 0.1, "maximum": 168, "default": 24},
            },
            "required": ["node"], "additionalProperties": False,
        },
    },
    {
        "name": "explain_score",
        "description": "解释一台节点的分数：cpu/runq/mem/net 四项归一化分数（0~1，越高越好）与权重、加权总分、"
                       "四个二次调整因子（f_inflight 在途、f_timeout 超时、f_slow 慢启动、f_pred 负载预测）、"
                       "最终权重和选中概率，以及剔除状态。回答“为什么分数低/为什么不派任务给它”时调用。",
        "parameters": {
            "type": "object",
            "properties": {"node": {"type": "string"}},
            "required": ["node"], "additionalProperties": False,
        },
    },
    {
        "name": "get_task_stats",
        "description": "任务统计（按分配时间落在最近 hours 小时内）：各状态任务数（done 完成、failed 失败、timeout 超时、"
                       "void 作废……）、超时细分（exec_timeouts 执行超时、unconfirmed 确认超时）、"
                       "完成时间 P50/P99/最大值（毫秒，从 Worker 收到到完成）。node 为空时给出全部节点合计和按节点分组。",
        "parameters": {
            "type": "object",
            "properties": {
                "node": {"type": "string", "description": "节点 id，留空表示全部节点"},
                "hours": {"type": "number", "minimum": 0.1, "maximum": 168, "default": 1},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "get_diagnoses",
        "description": "最近的 AI 异常诊断（严重度、一句话结论、首要原因、触发指标），新的在前，最多 20 条。node 为空表示全部节点。",
        "parameters": {
            "type": "object",
            "properties": {
                "node": {"type": "string"},
                "hours": {"type": "number", "minimum": 0.1, "maximum": 168, "default": 24},
            },
            "additionalProperties": False,
        },
    },
]
SPEC_BY_NAME = {s["name"]: s for s in SPECS}


def openai_tools() -> list[dict[str, Any]]:
    return [{"type": "function", "function": s} for s in SPECS]


def local_ts(ms: int | None, fmt: str = "%Y-%m-%d %H:%M:%S") -> str | None:
    if ms is None:
        return None
    return datetime.fromtimestamp(ms / 1000).strftime(fmt)


def _num(v: Any) -> Any:
    """Redis Hash 里的字符串转数字（保留 4 位有效小数）；转不了原样返回。"""
    if not isinstance(v, str):
        return v
    try:
        x = float(v)
    except ValueError:
        return v
    if not math.isfinite(x):
        return None
    return int(x) if x.is_integer() and "." not in v else round(x, 4)


def _f(v: Any) -> float | None:
    try:
        x = float(v)
    except (TypeError, ValueError):
        return None
    return x if math.isfinite(x) else None


def quantile(sorted_xs: list[float], q: float) -> float | None:
    """最近秩分位数（与 Manager 直方图口径无关，任务完成时间用）。"""
    if not sorted_xs:
        return None
    k = max(0, min(len(sorted_xs) - 1, math.ceil(q * len(sorted_xs)) - 1))
    return sorted_xs[k]


def task_summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    states: dict[str, int] = {}
    ms: list[float] = []
    for r in rows:
        states[r["state"]] = states.get(r["state"], 0) + 1
        if r.get("exec_ms") is not None:
            ms.append(float(r["exec_ms"]))
    ms.sort()
    # task 表的 timeout 合并了两种：Worker 没确认收到（确认超时，t_recv 为空）和收到后执行超时
    unconfirmed = sum(1 for r in rows if r["state"] == "timeout" and r.get("no_recv"))
    out: dict[str, Any] = {"tasks": len(rows), "states": dict(sorted(states.items())),
                           "done": states.get("done", 0), "failed": states.get("failed", 0),
                           "timeouts": states.get("timeout", 0),
                           "exec_timeouts": states.get("timeout", 0) - unconfirmed, "unconfirmed": unconfirmed}
    if ms:
        out.update(p50_ms=round(quantile(ms, 0.5)), p99_ms=round(quantile(ms, 0.99)), max_ms=round(ms[-1]))
    return out


def bucketize(points: list[tuple[int, float]], step_ms: int, agg: str = "avg") -> list[tuple[int, float]]:
    """按 step_ms 分桶（floor），桶内平均或取最大；输出按时间升序。"""
    buckets: dict[int, list[float]] = {}
    for t, v in points:
        buckets.setdefault(t - t % step_ms, []).append(v)
    out = []
    for b in sorted(buckets):
        xs = buckets[b]
        out.append((b, max(xs) if agg == "max" else sum(xs) / len(xs)))
    return out


def series_view(points: list[tuple[int, float]], with_date: bool) -> dict[str, Any]:
    if not points:
        return {"points": [], "note": "这段时间没有数据"}
    vals = [v for _, v in points]
    fmt = "%m-%d %H:%M" if with_date else "%H:%M:%S"
    return {"points": [[local_ts(t, fmt), round(v, 4)] for t, v in points],
            "min": round(min(vals), 4), "avg": round(sum(vals) / len(vals), 4), "max": round(max(vals), 4)}


class Tools:
    def __init__(self, cfg: Config, redis: Redis, mysql: MySQL, clock: Callable[[], float] = time.time):
        self.cfg = cfg
        self.redis = redis
        self.mysql = mysql
        self.clock = clock
        self.handlers: dict[str, Callable[..., Awaitable[dict[str, Any]]]] = {
            "list_nodes": self.list_nodes, "get_node_metrics": self.get_node_metrics,
            "get_node_events": self.get_node_events, "explain_score": self.explain_score,
            "get_task_stats": self.get_task_stats, "get_diagnoses": self.get_diagnoses,
        }

    def now_ms(self) -> int:
        return int(self.clock() * 1000)

    async def call(self, name: str, args: dict[str, Any] | str | None) -> dict[str, Any]:
        """按名字调用工具。参数可以是 JSON 字符串（函数调用）或 dict（MCP）。"""
        h = self.handlers.get(name)
        if h is None:
            return {"error": f"没有工具 {name}", "tools": list(self.handlers)}
        if isinstance(args, str):
            try:
                args = json.loads(args) if args.strip() else {}
            except json.JSONDecodeError as e:
                return {"error": f"参数不是合法 JSON：{e}"}
        args = dict(args or {})
        allowed = set(SPEC_BY_NAME[name]["parameters"]["properties"])
        extra = set(args) - allowed
        if extra:
            return {"error": f"未知参数 {sorted(extra)}，可用参数 {sorted(allowed)}"}
        try:
            return await h(**args)
        except TypeError as e:
            return {"error": f"参数错误：{e}"}
        except Exception as e:  # 数据源不可用等
            log.warning("工具 %s 出错：%s: %s", name, type(e).__name__, e)
            return {"error": f"{type(e).__name__}: {e}"[:300]}

    # ---------- 节点集合 ----------
    async def known_nodes(self, with_mysql: bool = False) -> list[str]:
        nodes: set[str] = set()
        try:
            nodes.update(await self.redis.node_ids())
        except Exception as e:
            log.warning("读节点列表失败：%s", e)
        if with_mysql:
            try:
                nodes.update(await self.mysql.node_ids())
            except Exception:
                pass
        return sorted(nodes)

    async def _check_node(self, node: Any, with_mysql: bool = False) -> dict[str, Any] | None:
        if not isinstance(node, str) or not node:
            return {"error": "node 不能为空", "nodes": await self.known_nodes(with_mysql)}
        known = await self.known_nodes(with_mysql)
        if node not in known:
            return {"error": f"未知节点 {node}", "nodes": known}
        return None

    @staticmethod
    def _clamp(v: Any, lo: float, hi: float, default: float) -> float:
        x = _f(v)
        if x is None:
            x = default
        return max(lo, min(hi, x))

    # ---------- 工具 ----------
    async def list_nodes(self) -> dict[str, Any]:
        nodes = await self.redis.node_ids()
        hashes = await self.redis.node_hashes(nodes)
        try:
            preds = await self.redis.get_preds()
        except Exception:
            preds = {}
        now = self.now_ms()
        out = []
        for n in nodes:
            h = hashes.get(n, {})
            last = _f(h.get("last_seen_ms"))
            item: dict[str, Any] = {"node": n}
            for k in ("state", "state_reason", "score", "prob", "cpu_busy", "mem_avail_ratio", "disk_util",
                      "runq_p99_us", "evicted", "evict_reason", "inflight_tasks", "f_inflight", "f_timeout",
                      "f_slow", "f_pred", "pred_cpu", "timeout_count", "ncpu", "best_nic"):
                if k in h and h[k] != "":
                    item[k] = _num(h[k])
            if last is not None:
                item["last_report_age_s"] = round(max(0, now - last) / 1000, 1)
                item["online"] = now - last <= 3000
            if n in preds:
                item["predict"] = preds[n]
            out.append(item)
        return {"ts": local_ts(now), "count": len(out), "online": sum(1 for x in out if x.get("online")),
                "nodes": out,
                "note": "score 和 prob 越高越空闲；prob 是下一个任务派给它的概率；last_report_age_s > 3 视为失联"}

    async def get_node_metrics(self, node: str, metric: str, minutes: float = 10) -> dict[str, Any]:
        err = await self._check_node(node, with_mysql=True)
        if err:
            return err
        if metric not in ALL_METRICS:
            return {"error": f"未知指标 {metric}", "metrics": list(ALL_METRICS)}
        minutes = int(self._clamp(minutes, 1, 1440, 10))
        now = self.now_ms()
        since = now - minutes * 60000
        if minutes <= 60:
            return await self._metrics_redis(node, metric, minutes, since, now)
        return await self._metrics_mysql(node, metric, minutes, since, now)

    async def _metrics_redis(self, node: str, metric: str, minutes: int, since: int, now: int) -> dict[str, Any]:
        raw = await self.redis.node_ts(node, since)
        series: dict[str, list[tuple[int, float]]] = {}
        for m in raw:
            try:
                s = json.loads(m)
            except json.JSONDecodeError:
                continue
            ts = int(_f(s.get("ts_ms")) or 0)
            if metric in NIC_METRICS:
                for nic in s.get("nics") or []:
                    v = _f(nic.get(metric)) if metric in nic else 0.0  # proto JSON 省略 0
                    if v is not None:
                        series.setdefault(nic.get("name") or "?", []).append((ts, v))
            else:
                key = {"cpu": "cpu_busy", "mem_avail": "mem_avail_ratio", "disk": "disk_util",
                       "runq_p99_us": "runq_p99_us"}[metric]
                if metric == "runq_p99_us" and key not in s:
                    continue  # 没上报调度延迟
                v = _f(s.get(key, 0))
                if v is not None:
                    series.setdefault(metric, []).append((ts, v))
        step_s = max(1, math.ceil(minutes * 60 / MAX_POINTS))
        agg = "max" if metric == "runq_p99_us" else "avg"
        out = {name: series_view(bucketize(pts, step_s * 1000, agg), False) for name, pts in sorted(series.items())}
        res: dict[str, Any] = {"node": node, "metric": metric, "source": "Redis 每秒样本",
                               "start": local_ts(since), "end": local_ts(now), "step_s": step_s,
                               "agg": "桶内最大" if agg == "max" else "桶内平均", "series": out}
        if not out:
            res["note"] = "这段时间没有样本（节点可能失联，或没有上报该指标）"
        return res

    async def _metrics_mysql(self, node: str, metric: str, minutes: int, since: int, now: int) -> dict[str, Any]:
        col = MYSQL_COLUMNS.get(metric)
        if col is None:
            return {"error": f"5 分钟聚合里没有 {metric}，这个指标最多查最近 60 分钟"}
        rows = await self.mysql.metrics_5m(node, since, now)
        pts = [(r["ts_ms"], float(r[col])) for r in rows if r.get(col) is not None]
        step_ms = 300000 * max(1, math.ceil(len(pts) / MAX_POINTS))
        agg = "max" if metric == "runq_p99_us" else "avg"
        name = "所有网卡平均" if metric in NIC_METRICS else metric
        res = {"node": node, "metric": metric, "source": f"MySQL 5 分钟聚合（列 {col}）",
               "start": local_ts(since), "end": local_ts(now), "step_s": step_ms // 1000,
               "series": {name: series_view(bucketize(pts, step_ms, agg), True)}}
        if metric == "cpu":
            mx = [(r["ts_ms"], float(r["cpu_max"])) for r in rows if r.get("cpu_max") is not None]
            res["series"]["cpu_max"] = series_view(bucketize(mx, step_ms, "max"), True)
        return res

    async def get_node_events(self, node: str, hours: float = 24) -> dict[str, Any]:
        err = await self._check_node(node, with_mysql=True)
        if err:
            return err
        hours = self._clamp(hours, 0.1, 168, 24)
        now = self.now_ms()
        since = now - int(hours * 3600000)
        source = "MySQL node_event"
        try:
            evs = await self.mysql.node_events(node, since, now + 1, 100)
        except Exception as e:
            log.warning("查 node_event 失败，改读 stream:events：%s", e)
            evs = [e for e in await self.redis.events_since(since, node, 100) if e["kind"] != "diagnosis"]
            source = "Redis stream:events（MySQL 不可用）"
        counts: dict[str, int] = {}
        for e in evs:
            counts[e["kind"]] = counts.get(e["kind"], 0) + 1
        return {"node": node, "hours": hours, "source": source, "count": len(evs), "by_kind": counts,
                "events": [{"ts": local_ts(e["ts_ms"]), "kind": e["kind"], "detail": e["detail"]} for e in evs],
                "note": "evict 的 reason：lost 上报中断、probe 探测失败、cpu/mem/runq/net 指标越过剔除线"}

    async def explain_score(self, node: str) -> dict[str, Any]:
        err = await self._check_node(node)
        if err:
            return err
        h = (await self.redis.node_hashes([node])).get(node, {})
        w = self.cfg.chat.score_weights
        parts = {}
        for k, field_ in (("cpu", "s_cpu"), ("runq", "s_runq"), ("mem", "s_mem"), ("net", "s_net")):
            parts[k] = {"score": _num(h.get(field_, "")), "weight": w.get(k, 0)}
        wsum = sum(w.values()) or 1
        recomputed = None
        if all(isinstance(p["score"], (int, float)) for p in parts.values()):
            recomputed = round(sum(p["score"] * p["weight"] for p in parts.values()) / wsum, 4)
        factors = {k: _num(h.get(k, "1")) for k in ("f_inflight", "f_timeout", "f_slow", "f_pred")}
        score = _num(h.get("score", ""))
        weight = None
        if isinstance(score, (int, float)) and all(isinstance(v, (int, float)) for v in factors.values()):
            weight = round(score * math.prod(factors.values()), 4)
        res: dict[str, Any] = {
            "node": node, "state": h.get("state"), "state_reason": h.get("state_reason") or None,
            "evicted": h.get("evicted") == "1", "evict_reason": h.get("evict_reason") or None,
            "parts": parts, "score": score, "score_recomputed": recomputed, "best_nic": h.get("best_nic") or None,
            "factors": factors, "weight": weight, "prob": _num(h.get("prob", "")),
            "raw": {k: _num(h[k]) for k in ("cpu_busy", "runq_p99_us", "mem_avail_ratio", "inflight_tasks",
                                             "timeout_count", "unconfirmed", "pred_cpu") if k in h},
            "rules": ("score = Σ 权重 × 分项 / Σ 权重；cpu 0.2→1 分、0.9→0 分；runq P99 取对数，100µs→1、2000µs→0；"
                      "mem 可用比例 0.5→1、0.1→0；net 取最好的一块网卡（RTT、超时率、重传率三项最小值）。"
                      "weight = score × f_inflight × f_timeout × f_slow × f_pred；prob = weight / 全部候选 weight 之和；"
                      "剔除、急救、失联的节点不参与选择，prob 为 0。f_pred < 1 表示预测 30 秒后 CPU 超过 0.7。"),
        }
        if recomputed is not None and isinstance(score, (int, float)) and abs(recomputed - score) > 0.01:
            res["warning"] = "按配置权重重算的分数与 Manager 的不一致，权重配置可能与 manager.yaml 不同"
        return res

    async def get_task_stats(self, node: str | None = None, hours: float = 1) -> dict[str, Any]:
        if node:
            err = await self._check_node(node, with_mysql=True)
            if err:
                return err
        hours = self._clamp(hours, 0.1, 168, 1)
        now = self.now_ms()
        since = now - int(hours * 3600000)
        rows = await self.mysql.task_rows(node or None, since, now + 1)
        res: dict[str, Any] = {"hours": hours, "start": local_ts(since), "end": local_ts(now),
                               "node": node or "全部", **task_summary(rows),
                               "note": "完成时间 = Worker 收到任务到完成，只统计 done；timeouts = exec_timeouts（收到后 300 秒没完成）"
                                       " + unconfirmed（分配后 30 秒 Worker 没确认收到）"}
        if not node:
            by: dict[str, list[dict[str, Any]]] = {}
            for r in rows:
                by.setdefault(r["node_id"], []).append(r)
            res["by_node"] = {n: task_summary(rs) for n, rs in sorted(by.items())}
        return res

    async def get_diagnoses(self, node: str | None = None, hours: float = 24) -> dict[str, Any]:
        if node:
            err = await self._check_node(node, with_mysql=True)
            if err:
                return err
        hours = self._clamp(hours, 0.1, 168, 24)
        rows = await self.mysql.list_diagnoses(node or None, None, hours, 20)
        out = []
        for r in rows:
            out.append({"id": r["id"], "ts": local_ts(r["ts"]), "node": r["node"], "severity": r["severity"],
                        "summary": r["summary"], "trigger": r.get("label") or r.get("metric"),
                        "model": r.get("model"), "llm": r.get("llm")})
        if out:  # 首要原因只取前 5 条的完整行，避免结果过长
            for item in out[:5]:
                full = await self.mysql.get_diagnosis(item["id"])
                causes = ((full or {}).get("report") or {}).get("causes") or []
                if causes:
                    item["top_cause"] = causes[0].get("cause")
        return {"hours": hours, "node": node or "全部", "count": len(out), "diagnoses": out}


def truncate_result(obj: dict[str, Any], max_bytes: int) -> str:
    """工具结果转 JSON；过长时把各序列的 points 抽稀（保留首尾），仍过长就硬截断。"""
    text = json.dumps(obj, ensure_ascii=False, separators=(",", ":"), default=str)
    if len(text.encode()) <= max_bytes:
        return text
    o = json.loads(text)

    def thin(x: Any) -> Any:
        if isinstance(x, dict):
            return {k: thin(v) for k, v in x.items()}
        if isinstance(x, list) and len(x) > 20:
            keep = x[:10] + x[-10:]
            return keep
        return x

    o = thin(o)
    o["truncated"] = True
    text = json.dumps(o, ensure_ascii=False, separators=(",", ":"))
    if len(text.encode()) > max_bytes:
        text = text.encode()[:max_bytes].decode(errors="ignore") + "…(truncated)"
    return text


def result_summary(name: str, res: dict[str, Any]) -> str:
    """工具结果的一行概括（看板显示）。"""
    if "error" in res:
        return f"错误：{res['error']}"
    if name == "list_nodes":
        return f"{res.get('count', 0)} 台节点，在线 {res.get('online', 0)}"
    if name == "get_node_metrics":
        s = res.get("series") or {}
        pts = sum(len(v.get("points", [])) for v in s.values())
        return f"{res.get('node')} {res.get('metric')}：{len(s)} 条序列，{pts} 个点"
    if name in ("get_node_events", "get_diagnoses"):
        return f"{res.get('count', 0)} 条"
    if name == "explain_score":
        return f"{res.get('node')} score {res.get('score')} prob {res.get('prob')}"
    if name == "get_task_stats":
        return f"{res.get('tasks', 0)} 个任务，超时 {res.get('timeouts', 0)}"
    return "ok"
