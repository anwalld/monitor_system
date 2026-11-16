"""每日报告（M9）：代码从 MySQL 算出前一天的数字，LLM 只负责成文，正文里每个数字都要能在 stats 里找到。

日界是服务器本地时间的自然日（容器挂 /etc/localtime）；MySQL 时间列存 UTC，查询时换成 UTC 区间。
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import re
import time
from datetime import date, datetime, timedelta
from typing import Any, Callable

from .config import Config
from .llm import LlmClient, LlmUnavailable
from .store import MySQL
from .tools import local_ts, task_summary

log = logging.getLogger("lmai.report")

REPORT_SYSTEM = """你是运维日报的撰写助手。下面的 JSON 是调度系统前一天的统计，所有数字都已由程序算好。
请写一份中文 Markdown 日报，200–400 字：先一句话总体结论，再分节点说明可用率、剔除与急救、任务完成时间和超时，
然后概括 AI 诊断，最后给 1–3 条运维建议。

严格要求：
1. 只能使用 JSON 里出现的数字，原样引用；小数可以保留更少的小数位，整数必须原样写出，不要取整到整十、整百、整千
   （例如 310002 要写成 310002 ms，或换算成 310 秒）。不要自己计算任何新数字（不要算差值、比例、合计、平均）。
2. 百分数用 JSON 里带 _pct 的字段；毫秒字段可以写成“xx ms”或除以 1000 写成秒；avail_s 可以用 avail_h（小时）。
   timeouts = exec_timeouts（Worker 收到后执行超时）+ unconfirmed（分配后 Worker 没确认收到，常见于 token 错误或网络断开），要分开说明。
5. 正文里用中文称呼（执行超时、确认超时、可用率、完成时间），不要写 JSON 字段名。
3. 不要写日期以外的时间推算；不要用阿拉伯数字给列表编号以外的东西计数，例如写“两台节点”而不是“2 台节点”，除非 JSON 里有这个数。
4. 不要输出代码块，不要复述 JSON。"""

SEV = ("high", "medium", "low")


def day_bounds(day: str) -> tuple[int, int]:
    """本地自然日 → [起, 止) 毫秒时间戳。"""
    d = date.fromisoformat(day)
    start = datetime(d.year, d.month, d.day)
    end = start + timedelta(days=1)
    return int(start.timestamp() * 1000), int(end.timestamp() * 1000)


def yesterday(now: float | None = None) -> str:
    t = datetime.fromtimestamp(now if now is not None else time.time())
    return (t.date() - timedelta(days=1)).isoformat()


def _tz_name() -> str:
    return time.strftime("%Z") or "local"


async def compute_stats(mysql: MySQL, day: str) -> dict[str, Any]:
    since, until = day_bounds(day)
    day_s = (until - since) / 1000
    rows5m = await mysql.metrics_5m(None, since, until)
    evs = await mysql.event_counts(since, until, ("evict", "rescue_start"))
    tasks = await mysql.task_rows(None, since, until)
    diags = await mysql.diagnoses_between(None, since, until, 500)

    per: dict[str, dict[str, Any]] = {}

    def node(n: str) -> dict[str, Any]:
        return per.setdefault(n, {"node": n, "avail_s": 0, "_cpu_w": 0.0, "_cpu_n": 0, "cpu_max": None,
                                  "evictions": 0, "rescues": 0})

    for r in rows5m:
        st = node(r["node_id"])
        n = int(r.get("samples") or 0)
        st["avail_s"] += n
        if r.get("cpu_avg") is not None and n > 0:
            st["_cpu_w"] += float(r["cpu_avg"]) * n
            st["_cpu_n"] += n
        if r.get("cpu_max") is not None:
            st["cpu_max"] = max(st["cpu_max"] or 0.0, float(r["cpu_max"]))
    for r in evs:
        node(r["node_id"])["evictions" if r["kind"] == "evict" else "rescues"] += int(r["n"])
    by_task: dict[str, list[dict[str, Any]]] = {}
    for t in tasks:
        by_task.setdefault(t["node_id"], []).append(t)
        node(t["node_id"])
    nodes = []
    for n in sorted(per):
        st = per[n]
        avail = min(st["avail_s"], int(day_s))
        item: dict[str, Any] = {"node": n, "avail_s": avail, "avail_h": round(avail / 3600, 2),
                                "avail_pct": round(avail / day_s * 100, 2)}
        if st["_cpu_n"]:
            item["cpu_avg_pct"] = round(st["_cpu_w"] / st["_cpu_n"] * 100, 1)
        if st["cpu_max"] is not None:
            item["cpu_max_pct"] = round(st["cpu_max"] * 100, 1)
        item["evictions"], item["rescues"] = st["evictions"], st["rescues"]
        ts = task_summary(by_task.get(n, []))
        ts.pop("states", None)
        item.update(ts)
        if item["tasks"]:
            item["timeout_pct"] = round(item["timeouts"] / item["tasks"] * 100, 2)
        nodes.append(item)

    totals = task_summary(tasks)
    states = totals.pop("states")
    totals.update(nodes=len(nodes), evictions=sum(x["evictions"] for x in nodes),
                  rescues=sum(x["rescues"] for x in nodes), states=states)
    if totals["tasks"]:
        totals["timeout_pct"] = round(totals["timeouts"] / totals["tasks"] * 100, 2)

    dsum: dict[str, Any] = {"total": len(diags), **{s: 0 for s in SEV}, "by_node": {}, "llm_ok": 0}
    triggers: dict[str, int] = {}
    for d in diags:
        dsum[d["severity"]] = dsum.get(d["severity"], 0) + 1
        dsum["by_node"][d["node"]] = dsum["by_node"].get(d["node"], 0) + 1
        dsum["llm_ok"] += 1 if d.get("llm") == "ok" else 0
        key = d.get("label") or d.get("metric") or "?"
        triggers[key] = triggers.get(key, 0) + 1
    dsum["items"] = [{"time": local_ts(d["ts"], "%H:%M"), "node": d["node"], "severity": d["severity"],
                      "summary": d.get("summary")} for d in sorted(diags, key=lambda x: x["ts"])[:20]]
    return {"day": day, "tz": _tz_name(), "day_s": int(day_s), "nodes": nodes, "totals": totals,
            "diagnoses": dsum,
            "triggers": [{"trigger": k, "count": v} for k, v in sorted(triggers.items(), key=lambda kv: -kv[1])]}


# ---------- 数字核对 ----------
_NUM = re.compile(r"(?<![\w.\-])\d+(?:\.\d+)?(?![\w\-])")
_STRIP = [
    re.compile(r"\d{4}-\d{1,2}-\d{1,2}"),                 # 2026-11-15
    re.compile(r"\d{4}\s*年\s*\d{1,2}\s*月\s*\d{1,2}\s*日"),  # 2026 年 11 月 15 日
    re.compile(r"\d{1,2}\s*月\s*\d{1,2}\s*日"),
    re.compile(r"\d{4}\s*年"),
    re.compile(r"\d{1,2}:\d{2}(?::\d{2})?"),              # 09:00
    re.compile(r"(?m)^\s*\d+[.)、]\s"),                    # 有序列表编号
    re.compile(r"\b[Pp](?:50|90|95|99)\b"),               # P50、P99
]


def extract_numbers(text: str, names: list[str]) -> list[str]:
    for n in sorted(names, key=len, reverse=True):  # 节点名、网卡名里的数字不算
        if n:
            text = text.replace(n, " ")
    text = re.sub(r"(?<=\d),(?=\d{3}\b)", "", text)    # 千分位
    for p in _STRIP:
        text = p.sub(" ", text)
    return _NUM.findall(text)


def _values(obj: Any, key: str = "") -> list[tuple[str, float]]:
    out: list[tuple[str, float]] = []
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.extend(_values(v, k))
    elif isinstance(obj, list):
        for v in obj:
            out.extend(_values(v, key))
    elif isinstance(obj, (int, float)) and not isinstance(obj, bool) and math.isfinite(obj):
        out.append((key, float(obj)))
    return out


def allowed_values(stats: dict[str, Any]) -> list[float]:
    """stats 里所有数字，以及常见的换算写法（毫秒→秒、秒→小时/分钟、比例→百分数）。"""
    vals: set[float] = set()
    for k, v in _values({kk: vv for kk, vv in stats.items() if kk not in ("llm", "mismatched", "model", "tokens", "llm_text")}):
        vals.add(v)
        if k.endswith("_ms"):
            vals.add(v / 1000)
        if k.endswith("_s"):
            vals.update((v / 3600, v / 60))
        if k.endswith("_pct"):
            vals.add(100 - v)  # “不可用 0.35%”这种写法允许
    return sorted(vals)


def _decimals(s: str) -> int:
    return len(s.split(".")[1]) if "." in s else 0


def check_numbers(text: str, stats: dict[str, Any]) -> list[str]:
    """正文里找不到出处的数字（按正文写出的小数位数对 stats 的值取整后比较）。"""
    names = [n["node"] for n in stats.get("nodes", [])] + list((stats.get("diagnoses") or {}).get("by_node", {}))
    vals = allowed_values(stats)
    bad = []
    for s in extract_numbers(text, names):
        x, d = float(s), _decimals(s)
        if not any(abs(round(v, d) - x) < 1e-9 for v in vals):
            bad.append(s)
    return bad


def render_table(stats: dict[str, Any], note: str = "") -> str:
    t = stats.get("totals", {})
    lines = [f"# {stats['day']} 运行日报", ""]
    if note:
        lines += [f"> {note}", ""]
    lines.append(f"节点 {t.get('nodes', 0)} 台；任务 {t.get('tasks', 0)} 个，完成 {t.get('done', 0)}，"
                 f"失败 {t.get('failed', 0)}，超时 {t.get('timeouts', 0)}（执行超时 {t.get('exec_timeouts', 0)}，"
                 f"确认超时 {t.get('unconfirmed', 0)}）；剔除 {t.get('evictions', 0)} 次，"
                 f"急救 {t.get('rescues', 0)} 次；AI 诊断 {stats.get('diagnoses', {}).get('total', 0)} 条。")
    lines += ["", "| 节点 | 可用率 % | CPU 平均 % | CPU 最大 % | 剔除 | 急救 | 任务 | 超时 | P50 ms | P99 ms |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    dash = "—"
    for n in stats.get("nodes", []):
        lines.append(f"| {n['node']} | {n.get('avail_pct', dash)} | {n.get('cpu_avg_pct', dash)} | "
                     f"{n.get('cpu_max_pct', dash)} | {n['evictions']} | {n['rescues']} | {n.get('tasks', 0)} | "
                     f"{n.get('timeouts', 0)} | {n.get('p50_ms', dash)} | {n.get('p99_ms', dash)} |")
    d = stats.get("diagnoses", {})
    if d.get("total"):
        lines += ["", f"AI 诊断：高 {d.get('high', 0)}、中 {d.get('medium', 0)}、低 {d.get('low', 0)}。", ""]
        for it in d.get("items", [])[:10]:
            lines.append(f"- {it['time']} {it['node']}［{it['severity']}］{it.get('summary') or ''}")
    return "\n".join(lines) + "\n"


class Reporter:
    def __init__(self, cfg: Config, mysql: MySQL, llm: LlmClient, clock: Callable[[], float] = time.time):
        self.cfg = cfg
        self.mysql = mysql
        self.llm = llm
        self.clock = clock
        self.last: dict[str, Any] | None = None
        self.scheduler: Any = None
        self._lock = asyncio.Lock()

    async def generate(self, day: str) -> dict[str, Any]:
        async with self._lock:
            stats = await compute_stats(self.mysql, day)
            text, reason, model, tokens, bad = "", "", None, 0, []
            try:
                text, res = await self.llm.complete(REPORT_SYSTEM, json.dumps(stats, ensure_ascii=False),
                                                    self.cfg.report.max_tokens)
                reason, model, tokens = res.reason, res.model, res.tokens
            except LlmUnavailable as e:
                reason = e.reason
            if reason == "ok":
                bad = check_numbers(text, stats)
                if bad:
                    reason = "mismatch"
                elif not text.strip():
                    reason = "error: 空正文"
            stats.update(llm=reason, model=model, tokens=tokens)
            if bad:
                stats["mismatched"] = bad[:50]
                stats["llm_text"] = text[:4000]  # 被否决的正文留着便于排查
            if reason == "ok":
                markdown = text.strip() + "\n"
            else:
                note = {"mismatch": f"LLM 正文里有 {len(bad)} 个数字对不上统计，改为只发数字表格"}.get(reason, "")
                markdown = render_table(stats, note)
            await self.mysql.put_report(day, stats, markdown)
            self.last = {"day": day, "llm": reason, "ts_ms": int(self.clock() * 1000)}
            log.info("日报 %s 已生成（%s，%d tokens%s）", day, reason, tokens,
                     f"，不匹配 {bad[:5]}" if bad else "")
            return {"day": day, "stats": stats, "markdown": markdown}

    # ---------- 定时 ----------
    def _at(self) -> tuple[int, int]:
        h, _, m = self.cfg.report.at.partition(":")
        return int(h), int(m or 0)

    async def _job(self) -> None:
        day = yesterday(self.clock())
        for attempt in range(3):
            try:
                await self.generate(day)
                return
            except Exception as e:
                log.warning("生成日报 %s 失败（第 %d 次）：%s: %s", day, attempt + 1, type(e).__name__, e)
                if attempt < 2:
                    await asyncio.sleep(3600)

    async def catch_up(self) -> None:
        """启动时：已过生成时刻而昨天的日报还没有，就补生成一次。"""
        h, m = self._at()
        now = datetime.fromtimestamp(self.clock())
        if (now.hour, now.minute) < (h, m):
            return
        day = yesterday(self.clock())
        try:
            if await self.mysql.get_report(day) is None:
                log.info("补生成昨天（%s）的日报", day)
                await self.generate(day)
        except Exception as e:
            log.warning("检查/补生成日报失败：%s: %s", type(e).__name__, e)

    def start(self) -> None:
        if not self.cfg.report.enabled:
            return
        from apscheduler.schedulers.asyncio import AsyncIOScheduler  # noqa: PLC0415
        from apscheduler.triggers.cron import CronTrigger  # noqa: PLC0415
        h, m = self._at()
        self.scheduler = AsyncIOScheduler()
        self.scheduler.add_job(self._job, CronTrigger(hour=h, minute=m), id="daily_report",
                               misfire_grace_time=3600, coalesce=True, max_instances=1)
        self.scheduler.start()

    def stop(self) -> None:
        if self.scheduler is not None:
            self.scheduler.shutdown(wait=False)
            self.scheduler = None

    def health(self) -> dict[str, Any]:
        nxt = None
        if self.scheduler is not None:
            job = self.scheduler.get_job("daily_report")
            if job is not None and job.next_run_time is not None:
                nxt = job.next_run_time.strftime("%Y-%m-%d %H:%M:%S")
        return {"enabled": self.cfg.report.enabled, "at": self.cfg.report.at, "next_run": nxt,
                "last_day": (self.last or {}).get("day"), "last": self.last}
