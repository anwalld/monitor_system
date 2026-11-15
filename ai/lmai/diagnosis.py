"""服务编排：消费两个 Stream、检测、诊断队列、写 MySQL 和诊断事件；M9 加负载预测推送、问答工具、日报定时。"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import time
from collections import deque
from typing import Any, Callable

from .config import Config
from .context import SYSTEM_PROMPT, Anonymizer, build_context
from .detector import Anomaly, Detector
from .llm import LlmClient, LlmResult
from .predict import Predictor, Pusher
from .report import Reporter
from .store import MySQL, Redis, event_fields
from .tools import Tools

log = logging.getLogger("lmai")


class RateLog:
    """同一类错误 10 秒内只记一次。"""

    def __init__(self, period_s: float = 10):
        self.period = period_s
        self.last: dict[str, float] = {}
        self.suppressed: dict[str, int] = {}

    def warn(self, key: str, msg: str, *args: Any) -> None:
        now = time.monotonic()
        # 异常对象写成“类型: 信息”（TimeoutError 之类 str() 为空）
        args = tuple(f"{type(a).__name__}: {a}" if isinstance(a, BaseException) else a for a in args)
        if now - self.last.get(key, -1e9) >= self.period:
            n = self.suppressed.pop(key, 0)
            log.warning(msg + ("（此前 10 秒内另有 %d 次）" % n if n else ""), *args)
            self.last[key] = now
        else:
            self.suppressed[key] = self.suppressed.get(key, 0) + 1


def stats_report(a: Anomaly, res: LlmResult) -> dict[str, Any]:
    return {"severity": a.severity, "summary": a.describe(), "causes": [], "actions": [],
            "llm": res.reason, "attempts": res.attempts}


class Service:
    def __init__(self, cfg: Config, redis: Redis, mysql: MySQL, llm: LlmClient,
                 detector: Detector | None = None, clock: Callable[[], float] = time.time):
        self.cfg = cfg
        self.redis = redis
        self.mysql = mysql
        self.llm = llm
        self.det = detector or Detector(cfg.detect)
        self.clock = clock
        self.queue: asyncio.Queue[Anomaly] = asyncio.Queue(maxsize=cfg.diagnosis.queue_max)
        self.pending_rows: deque[dict[str, Any]] = deque(maxlen=200)
        self.errs = RateLog()
        self.diagnoses_total = 0
        self.dropped = 0
        self.samples_total = 0
        self.warmed = 0
        self.tasks: list[asyncio.Task] = []
        self._running = False
        # M9
        self.tools = Tools(cfg, redis, mysql, clock)
        self.predictor = Predictor.from_cfg(cfg.predict)
        self.pusher = Pusher(cfg.predict.manager_addr, cfg.predict.rpc_timeout_s)
        self.reporter = Reporter(cfg, mysql, llm, clock)
        self.push_ok = 0
        self.push_fail = 0
        self.last_push: dict[str, Any] | None = None

    def now_ms(self) -> int:
        return int(self.clock() * 1000)

    # ---------- 生命周期 ----------
    def start(self) -> None:
        self._running = True
        for coro in (self.run_metrics(), self.run_events(), self.run_diagnosis(), self.run_flush(),
                     self.run_predict(), self.reporter.catch_up()):
            self.tasks.append(asyncio.create_task(coro))
        try:
            self.reporter.start()
        except Exception as e:  # 定时器起不来不影响其他功能
            log.warning("日报定时任务启动失败：%s: %s", type(e).__name__, e)

    async def stop(self) -> None:
        self._running = False
        for t in self.tasks:
            t.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        self.tasks.clear()
        self.reporter.stop()
        await self.pusher.close()

    # ---------- 入队 ----------
    def handle(self, anomalies: list[Anomaly]) -> None:
        now = self.now_ms()
        for a in anomalies:
            log.info("异常 %s %s %s %s", a.node, a.rule, a.metric, a.severity)
            if not self.det.admit(a, now):
                continue
            if self.queue.full():  # 丢最旧的，保证最新的异常能诊断
                try:
                    old = self.queue.get_nowait()
                    self.queue.task_done()
                    self.dropped += 1
                    log.warning("诊断队列已满，丢弃 %s 的 %s", old.node, old.metric)
                except asyncio.QueueEmpty:
                    pass
            self.queue.put_nowait(a)

    # ---------- 消费 ----------
    async def warm(self) -> None:
        since = self.now_ms() - int(self.cfg.detect.window_s * 1000)
        for node in await self.redis.ts_nodes():
            samples = []
            for m in await self.redis.node_ts(node, since):
                try:
                    samples.append(json.loads(m))
                except json.JSONDecodeError:
                    pass
            self.warmed += self.det.warm(node, samples)
            if self.cfg.predict.enabled:  # 预测也用历史样本预热，重启后不用等 min_samples 秒
                for smp in sorted(samples, key=lambda x: float(x.get("ts_ms") or 0)):
                    try:
                        self.predictor.observe(node, int(float(smp.get("ts_ms") or 0)),
                                               float(smp.get("cpu_busy") or 0.0))
                    except (TypeError, ValueError):
                        pass
        log.info("检测窗口预热完成：%d 台节点，%d 条样本", self.det.node_count(), self.warmed)

    def on_metrics_entry(self, fields: dict[str, str]) -> None:
        try:
            nodes = json.loads(fields.get("nodes") or "[]")
        except json.JSONDecodeError:
            return
        now = self.now_ms()
        for s in nodes:
            self.samples_total += 1
            self.handle(self.det.on_sample(s, now))
            if self.cfg.predict.enabled:
                try:
                    self.predictor.observe(s.get("node_id") or "", int(float(s.get("ts_ms") or 0)),
                                           float(s.get("cpu_busy") or 0.0))
                except (TypeError, ValueError):
                    pass

    def on_event_entry(self, eid: str, fields: dict[str, str]) -> None:
        e = event_fields(eid, fields)
        if e["kind"] == "diagnosis":
            return
        self.handle(self.det.on_event(e["node"], e["kind"], e["ts_ms"], e["detail"], self.now_ms()))

    async def _consume(self, stream: str, skip_backlog: bool, on_entry: Callable[[str, dict], None]) -> None:
        ready, last = False, ">"
        while self._running:
            try:
                if not ready:
                    await self.redis.ensure_group(stream, skip_backlog)
                    if stream == "stream:metrics":
                        await self.warm()
                    # 先处理上次没确认的（崩溃时读到一半），再读新的
                    last = ">" if skip_backlog else "0"
                    ready = True
                items = await self.redis.read_group(stream, last, 100, 1000)
                if last == "0" and not items:
                    last = ">"
                    continue
                for eid, f in items:
                    try:
                        on_entry(eid, f)
                    except Exception:
                        log.exception("处理 %s %s 出错", stream, eid)
                await self.redis.ack(stream, [eid for eid, _ in items])
            except asyncio.CancelledError:
                raise
            except Exception as e:
                self.errs.warn(stream, "读 %s 失败：%s", stream, e)
                ready = False  # 重建 group（Redis 重启或 Stream 被删时 group 会消失）
                await asyncio.sleep(1)

    async def run_metrics(self) -> None:
        await self._consume("stream:metrics", True, lambda _id, f: self.on_metrics_entry(f))

    # ---------- 负载预测（M9） ----------
    async def predict_once(self) -> list[dict[str, Any]]:
        pc = self.cfg.predict
        now = self.now_ms()
        items = self.predictor.tick(now)
        if not items:
            return items
        try:
            await self.redis.set_preds({i["node_id"]: {k: i[k] for k in ("cpu_busy_30s", "factor", "cpu_now", "ts_ms")}
                                        for i in items}, max(1, math.ceil(pc.ttl_ms / 1000)))
        except Exception as e:
            self.errs.warn("pred", "写 ai:pred 失败：%s", e)
        if pc.push:
            try:
                await self.pusher.push(items, now, pc.ttl_ms)
                self.push_ok += 1
                self.last_push = {"ts_ms": now, "nodes": len(items), "ok": True}
            except Exception as e:
                self.push_fail += 1
                self.last_push = {"ts_ms": now, "nodes": len(items), "ok": False, "error": f"{type(e).__name__}"}
                self.errs.warn("push", "推送预测到 Manager 失败：%s", e)
        return items

    async def run_predict(self) -> None:
        while self._running:
            await asyncio.sleep(self.cfg.predict.interval_s)
            if not self.cfg.predict.enabled:
                continue
            try:
                await self.predict_once()
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("预测出错")

    def predict_health(self) -> dict[str, Any]:
        pc = self.cfg.predict
        tot = self.predictor.view(self.now_ms())["total"]
        return {"enabled": pc.enabled, "push": pc.enabled and pc.push, "manager": pc.manager_addr,
                "nodes": sum(1 for st in self.predictor.nodes.values() if st.latest is not None),
                "push_ok": self.push_ok, "push_fail": self.push_fail, "last_push": self.last_push,
                "mae_holt": tot["mae_holt"], "mae_naive": tot["mae_naive"], "points": tot["points"]}

    def report_health(self) -> dict[str, Any]:
        return self.reporter.health()

    async def run_events(self) -> None:
        await self._consume("stream:events", False, self.on_event_entry)

    # ---------- 诊断 ----------
    async def run_diagnosis(self) -> None:
        while self._running:
            a = await self.queue.get()
            try:
                await self.diagnose(a)
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("诊断 %s 出错", a.node)
            finally:
                self.queue.task_done()

    def _related(self, a: Anomaly) -> list[Anomaly]:
        return self.det.related(a, int(self.cfg.diagnosis.settle_s * 1000))

    async def gather_context(self, a: Anomaly) -> dict[str, Any]:
        lookback = self.now_ms() - self.cfg.diagnosis.events_lookback_s * 1000
        try:
            node_hash = await self.redis.node_hash(a.node)
        except Exception as e:
            self.errs.warn("hash", "读 node 哈希失败：%s", e)
            node_hash = {}
        try:
            transitions = await self.redis.node_events(a.node, lookback)
        except Exception as e:
            self.errs.warn("events", "读节点事件失败：%s", e)
            transitions = []
        try:
            timeouts = await self.mysql.task_timeouts(a.node, lookback)
        except Exception as e:
            self.errs.warn("task", "查任务超时失败：%s", e)
            timeouts = None
        return build_context(a, self._related(a), node_hash,
                             self.det.recent_metrics(a.node, self.cfg.diagnosis.recent_step_s),
                             transitions, timeouts, self.det.peer_median(a.node),
                             self.det.reporting(a.node, self.now_ms()))

    async def diagnose(self, a: Anomaly) -> dict[str, Any]:
        wait = (a.ts_ms + self.cfg.diagnosis.settle_s * 1000 - self.now_ms()) / 1000
        if wait > 0:
            await asyncio.sleep(wait)
        ctx = await self.gather_context(a)
        anon = Anonymizer(list(self.det.nodes) + [a.node], self.cfg.diagnosis.anonymize)
        res = await self.llm.diagnose(SYSTEM_PROMPT, anon.hide(ctx))
        if res.report is not None:
            report = anon.reveal(res.report.model_dump())
            report.update(llm="ok", attempts=res.attempts)
        else:
            report = stats_report(a, res)
        row = {
            "node": a.node, "ts_ms": self.now_ms(), "severity": report["severity"],
            "anomaly": {"trigger": a.to_dict(), "related": [x.to_dict() for x in self._related(a)],
                        "context": ctx},
            "report": report, "model": res.model if res.report is not None else None, "tokens": res.tokens,
        }
        did = await self._insert(row)
        detail = {"diag_id": did, "severity": row["severity"], "summary": report["summary"],
                  "metric": a.metric, "label": row["anomaly"]["trigger"]["label"], "rule": a.rule,
                  "model": row["model"], "llm": report["llm"]}
        try:
            await self.redis.add_event(a.node, "diagnosis", row["ts_ms"], detail)
        except Exception as e:
            self.errs.warn("xadd", "写诊断事件失败：%s", e)
        self.diagnoses_total += 1
        log.info("诊断 #%s %s %s：%s（%s）", did, a.node, row["severity"], report["summary"], report["llm"])
        return {"id": did, **row}

    async def _insert(self, row: dict[str, Any]) -> int | None:
        try:
            return await self.mysql.insert_diagnosis(row["node"], row["ts_ms"], row["severity"], row["anomaly"],
                                                     row["report"], row["model"], row["tokens"])
        except Exception as e:
            self.errs.warn("mysql", "写 ai_diagnosis 失败，稍后补写：%s", e)
            self.pending_rows.append(row)
            return None

    async def run_flush(self) -> None:
        while self._running:
            await asyncio.sleep(5)
            while self.pending_rows:
                row = self.pending_rows[0]
                try:
                    await self.mysql.insert_diagnosis(row["node"], row["ts_ms"], row["severity"], row["anomaly"],
                                                      row["report"], row["model"], row["tokens"])
                except Exception:
                    break
                self.pending_rows.popleft()
                log.info("补写诊断 %s 成功，剩 %d 条", row["node"], len(self.pending_rows))
