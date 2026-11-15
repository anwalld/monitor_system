"""Redis 与 MySQL 访问。MySQL 时间列与 Manager 一致，存 UTC。"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from datetime import datetime, timezone
from typing import Any

import aiomysql
import redis.asyncio as aioredis
from redis.exceptions import ResponseError

from .config import Config
from .llm import Selection

log = logging.getLogger("lmai.store")


def utc_dt(ms: int) -> datetime:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).replace(tzinfo=None)


def dt_ms(dt: datetime) -> int:
    return int(dt.replace(tzinfo=timezone.utc).timestamp() * 1000)


def parse_redis_uri(uri: str) -> tuple[str, int]:
    rest = uri[len("tcp://"):] if uri.startswith("tcp://") else uri
    if "://" in rest:
        raise ValueError(f"只支持 tcp:// 的 Redis 地址: {uri}")
    host, _, port = rest.rpartition(":")
    if not host:
        host, port = rest, "6379"
    return host, int(port or 6379)


class Redis:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        host, port = parse_redis_uri(cfg.redis_uri)
        self.r = aioredis.Redis(host=host, port=port, password=cfg.redis_password or None,
                                decode_responses=True, socket_timeout=5, socket_connect_timeout=2,
                                health_check_interval=30)

    def key(self, k: str) -> str:
        return self.cfg.key_prefix + k

    async def ping(self) -> bool:
        try:
            return bool(await self.r.ping())
        except Exception:
            return False

    async def ensure_group(self, stream: str, skip_backlog: bool) -> None:
        """创建 consumer group；skip_backlog 时把读位置移到末尾（停机期间的积压不再处理）。"""
        k = self.key(stream)
        try:
            await self.r.xgroup_create(k, self.cfg.redis_group, id="$", mkstream=True)
        except ResponseError as e:
            if "BUSYGROUP" not in str(e):
                raise
            if skip_backlog:
                await self.r.xgroup_setid(k, self.cfg.redis_group, id="$")

    async def read_group(self, stream: str, last: str, count: int, block_ms: int) -> list[tuple[str, dict]]:
        res = await self.r.xreadgroup(self.cfg.redis_group, self.cfg.redis_consumer,
                                      {self.key(stream): last}, count=count, block=block_ms)
        out: list[tuple[str, dict]] = []
        for _, items in res or []:
            out.extend(items)
        return out

    async def ack(self, stream: str, ids: list[str]) -> None:
        if ids:
            await self.r.xack(self.key(stream), self.cfg.redis_group, *ids)

    async def ts_nodes(self) -> list[str]:
        pre, out = self.key("node:"), []
        async for k in self.r.scan_iter(match=pre + "*:ts", count=200):
            out.append(k[len(pre):-3])
        return out

    async def node_ts(self, node: str, since_ms: int) -> list[str]:
        return await self.r.zrangebyscore(self.key(f"node:{node}:ts"), since_ms, "+inf")

    async def node_hash(self, node: str) -> dict[str, str]:
        return await self.r.hgetall(self.key(f"node:{node}"))

    async def node_events(self, node: str, since_ms: int, limit: int = 2000) -> list[dict[str, Any]]:
        items = await self.r.xrevrange(self.key("stream:events"), "+", f"{since_ms}-0", count=limit)
        out = []
        for eid, f in items:
            if f.get("node") != node or f.get("kind") == "diagnosis":
                continue
            out.append(event_fields(eid, f))
        out.reverse()
        return out[-30:]

    async def add_event(self, node: str, kind: str, ts_ms: int, detail: dict[str, Any]) -> str:
        return await self.r.xadd(self.key("stream:events"),
                                 {"node": node, "kind": kind, "ts_ms": str(ts_ms),
                                  "detail": json.dumps(detail, ensure_ascii=False, separators=(",", ":"))},
                                 maxlen=10000, approximate=True)

    # ---------- M9：问答工具与预测 ----------
    async def node_ids(self) -> list[str]:
        """有 node:{id} Hash 的节点（不含 :ts）。"""
        pre, out = self.key("node:"), []
        async for k in self.r.scan_iter(match=pre + "*", count=200):
            rest = k[len(pre):]
            if ":" not in rest:
                out.append(rest)
        return sorted(out)

    async def node_hashes(self, nodes: list[str]) -> dict[str, dict[str, str]]:
        p = self.r.pipeline(transaction=False)
        for n in nodes:
            p.hgetall(self.key(f"node:{n}"))
        res = await p.execute()
        return {n: h for n, h in zip(nodes, res) if h}

    async def events_since(self, since_ms: int, node: str | None = None, limit: int = 100,
                           scan_max: int = 5000) -> list[dict[str, Any]]:
        """stream:events 里 since_ms 之后的事件（新的在前），可按节点过滤。"""
        items = await self.r.xrevrange(self.key("stream:events"), "+", f"{since_ms}-0", count=scan_max)
        out = []
        for eid, f in items:
            if node and f.get("node") != node:
                continue
            out.append(event_fields(eid, f))
            if len(out) >= limit:
                break
        return out

    async def get_preds(self) -> dict[str, dict[str, Any]]:
        h = await self.r.hgetall(self.key("ai:pred"))
        out = {}
        for n, v in h.items():
            try:
                out[n] = json.loads(v)
            except json.JSONDecodeError:
                pass
        return out

    async def set_preds(self, preds: dict[str, dict[str, Any]], ttl_s: int) -> None:
        if not preds:
            return
        k = self.key("ai:pred")
        p = self.r.pipeline(transaction=True)
        p.delete(k)  # 只保留这一轮的节点，下线的节点不残留
        p.hset(k, mapping={n: json.dumps(v, separators=(",", ":")) for n, v in preds.items()})
        p.expire(k, ttl_s)
        await p.execute()

    async def get_selection(self) -> Selection:
        h = await self.r.hgetall(self.key("ai:settings"))
        vendor = h.get("vendor", "")
        key = await self.r.hget(self.key("ai:keys"), vendor) if vendor else None
        return Selection(vendor, h.get("model", ""), key or "")

    async def ui_keys(self) -> dict[str, str]:
        """看板里填的各厂商 Key（只在服务端使用，不返回给前端）。"""
        return await self.r.hgetall(self.key("ai:keys"))

    async def set_ui_key(self, vendor: str, key: str | None) -> None:
        if key:
            await self.r.hset(self.key("ai:keys"), vendor, key)
        else:
            await self.r.hdel(self.key("ai:keys"), vendor)

    async def set_selection(self, sel: Selection) -> None:
        await self.r.hset(self.key("ai:settings"),
                          mapping={"vendor": sel.vendor, "model": sel.model, "updated_ms": str(int(time.time() * 1000))})

    def _tokens_key(self) -> str:
        return self.key("ai:tokens:" + datetime.now().strftime("%Y%m%d"))

    async def used_today(self) -> int:
        return int(await self.r.get(self._tokens_key()) or 0)

    async def add(self, n: int) -> None:
        if n <= 0:
            return
        k = self._tokens_key()
        p = self.r.pipeline(transaction=False)
        p.incrby(k, n)
        p.expire(k, 2 * 86400)
        await p.execute()


def event_fields(eid: str, f: dict[str, str]) -> dict[str, Any]:
    try:
        detail = json.loads(f.get("detail") or "{}")
    except json.JSONDecodeError:
        detail = {"raw": f.get("detail")}
    try:
        ts = int(f.get("ts_ms") or eid.split("-")[0])
    except ValueError:
        ts = int(eid.split("-")[0])
    return {"id": eid, "node": f.get("node", ""), "kind": f.get("kind", ""), "ts_ms": ts, "detail": detail}


class MySQL:
    """aiomysql 连接池，失败后丢弃连接池，下次调用重建（MySQL 恢复后自动重连）。"""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.pool: aiomysql.Pool | None = None
        self._lock = asyncio.Lock()

    async def _pool(self) -> aiomysql.Pool:
        async with self._lock:
            if self.pool is None:
                self.pool = await asyncio.wait_for(aiomysql.create_pool(
                    host=self.cfg.mysql_host, port=self.cfg.mysql_port, user=self.cfg.mysql_user,
                    password=self.cfg.mysql_password, db=self.cfg.mysql_database, autocommit=True,
                    charset="utf8mb4", minsize=0, maxsize=4, connect_timeout=3, pool_recycle=3600), 5)
            return self.pool

    async def _drop(self) -> None:
        async with self._lock:
            p, self.pool = self.pool, None
        if p is not None:
            p.close()
            try:
                await asyncio.wait_for(p.wait_closed(), 2)
            except Exception:
                pass

    async def _run(self, sql: str, args: tuple = (), fetch: str = "all") -> Any:
        try:
            pool = await self._pool()
            async def go() -> Any:
                async with pool.acquire() as conn:
                    async with conn.cursor(aiomysql.DictCursor) as cur:
                        await cur.execute(sql, args)
                        if fetch == "lastrowid":
                            return cur.lastrowid
                        if fetch == "one":
                            return await cur.fetchone()
                        return await cur.fetchall()
            return await asyncio.wait_for(go(), 8)
        except Exception:
            await self._drop()
            raise

    async def ping(self) -> bool:
        try:
            await self._run("SELECT 1", fetch="one")
            return True
        except Exception:
            return False

    async def insert_diagnosis(self, node: str, ts_ms: int, severity: str, anomaly: dict, report: dict,
                               model: str | None, tokens: int) -> int:
        return await self._run(
            "INSERT INTO ai_diagnosis (node_id, ts, severity, anomaly, report, model, tokens) "
            "VALUES (%s, %s, %s, %s, %s, %s, %s)",
            (node, utc_dt(ts_ms), severity, json.dumps(anomaly, ensure_ascii=False),
             json.dumps(report, ensure_ascii=False), model, tokens), fetch="lastrowid")

    @staticmethod
    def _row(r: dict[str, Any], full: bool) -> dict[str, Any]:
        anomaly = json.loads(r["anomaly"]) if isinstance(r["anomaly"], str) else r["anomaly"]
        report = json.loads(r["report"]) if isinstance(r["report"], str) else r["report"]
        trig = (anomaly or {}).get("trigger", {})
        out = {"id": r["id"], "node": r["node_id"], "ts": dt_ms(r["ts"]), "severity": r["severity"],
               "summary": report.get("summary"), "metric": trig.get("metric"), "label": trig.get("label"),
               "rule": trig.get("rule"), "model": r["model"], "tokens": r["tokens"], "llm": report.get("llm")}
        if full:
            out["anomaly"], out["report"] = anomaly, report
        return out

    async def list_diagnoses(self, node: str | None, severity: str | None, hours: float, limit: int) -> list[dict]:
        where, args = ["ts >= %s"], [utc_dt(int((time.time() - hours * 3600) * 1000))]
        if node:
            where.append("node_id = %s")
            args.append(node)
        if severity:
            where.append("severity = %s")
            args.append(severity)
        rows = await self._run(
            f"SELECT * FROM ai_diagnosis WHERE {' AND '.join(where)} ORDER BY ts DESC, id DESC LIMIT %s",
            (*args, limit))
        return [self._row(r, False) for r in rows]

    async def get_diagnosis(self, did: int) -> dict | None:
        r = await self._run("SELECT * FROM ai_diagnosis WHERE id = %s", (did,), fetch="one")
        return self._row(r, True) if r else None

    async def task_timeouts(self, node: str, since_ms: int) -> dict[str, Any]:
        rows = await self._run(
            "SELECT task_id, t_alloc, t_recv FROM task WHERE node_id = %s AND state = 'timeout' "
            "AND t_alloc >= %s ORDER BY t_alloc DESC LIMIT 200", (node, utc_dt(since_ms)))
        return {"count": len(rows),
                "latest": [{"task_id": r["task_id"], "t_alloc_ms": dt_ms(r["t_alloc"]),
                            "received": r["t_recv"] is not None} for r in rows[:5]]}

    # ---------- M9：问答工具与日报 ----------
    async def node_ids(self) -> list[str]:
        rows = await self._run("SELECT node_id FROM node ORDER BY node_id")
        return [r["node_id"] for r in rows]

    async def metrics_5m(self, node: str | None, since_ms: int, until_ms: int) -> list[dict[str, Any]]:
        where, args = ["bucket_start >= %s", "bucket_start < %s"], [utc_dt(since_ms), utc_dt(until_ms)]
        if node:
            where.append("node_id = %s")
            args.append(node)
        rows = await self._run(
            f"SELECT * FROM node_metrics_5m WHERE {' AND '.join(where)} ORDER BY node_id, bucket_start",
            tuple(args))
        for r in rows:
            r["ts_ms"] = dt_ms(r.pop("bucket_start"))
        return rows

    async def node_events(self, node: str | None, since_ms: int, until_ms: int, limit: int = 100,
                          kinds: tuple[str, ...] = ()) -> list[dict[str, Any]]:
        where, args = ["ts >= %s", "ts < %s"], [utc_dt(since_ms), utc_dt(until_ms)]
        if node:
            where.append("node_id = %s")
            args.append(node)
        if kinds:
            where.append("kind IN (" + ",".join(["%s"] * len(kinds)) + ")")
            args.extend(kinds)
        rows = await self._run(
            f"SELECT node_id, ts, kind, detail FROM node_event WHERE {' AND '.join(where)} "
            f"ORDER BY ts DESC, id DESC LIMIT %s", (*args, limit))
        out = []
        for r in rows:
            d = r["detail"]
            if isinstance(d, (bytes, str)):
                try:
                    d = json.loads(d)
                except (json.JSONDecodeError, TypeError):
                    d = {"raw": str(d)}
            out.append({"node": r["node_id"], "ts_ms": dt_ms(r["ts"]), "kind": r["kind"], "detail": d or {}})
        return out

    async def event_counts(self, since_ms: int, until_ms: int, kinds: tuple[str, ...]) -> list[dict[str, Any]]:
        return await self._run(
            "SELECT node_id, kind, COUNT(*) AS n FROM node_event WHERE ts >= %s AND ts < %s AND kind IN ("
            + ",".join(["%s"] * len(kinds)) + ") GROUP BY node_id, kind",
            (utc_dt(since_ms), utc_dt(until_ms), *kinds))

    async def task_rows(self, node: str | None, since_ms: int, until_ms: int,
                        limit: int = 500000) -> list[dict[str, Any]]:
        """按 t_alloc 落在区间内的任务：node_id、state、no_recv（Worker 没确认收到）、exec_ms（done 的 t_done − t_recv，毫秒）。"""
        where, args = ["t_alloc >= %s", "t_alloc < %s"], [utc_dt(since_ms), utc_dt(until_ms)]
        if node:
            where.append("node_id = %s")
            args.append(node)
        return await self._run(
            "SELECT node_id, state, (t_recv IS NULL) AS no_recv, "
            "IF(state = 'done' AND t_recv IS NOT NULL AND t_done IS NOT NULL, "
            "TIMESTAMPDIFF(MICROSECOND, t_recv, t_done) / 1000, NULL) AS exec_ms "
            f"FROM task WHERE {' AND '.join(where)} LIMIT %s", (*args, limit))

    async def diagnoses_between(self, node: str | None, since_ms: int, until_ms: int,
                                limit: int = 200) -> list[dict[str, Any]]:
        where, args = ["ts >= %s", "ts < %s"], [utc_dt(since_ms), utc_dt(until_ms)]
        if node:
            where.append("node_id = %s")
            args.append(node)
        rows = await self._run(
            f"SELECT * FROM ai_diagnosis WHERE {' AND '.join(where)} ORDER BY ts DESC, id DESC LIMIT %s",
            (*args, limit))
        return [self._row(r, True) for r in rows]

    async def put_report(self, day: str, stats: dict[str, Any], markdown: str) -> None:
        await self._run(
            "INSERT INTO ai_daily_report (day, stats, markdown, created_at) VALUES (%s, %s, %s, %s) AS new "
            "ON DUPLICATE KEY UPDATE stats = new.stats, markdown = new.markdown, created_at = new.created_at",
            (day, json.dumps(stats, ensure_ascii=False), markdown, utc_dt(int(time.time() * 1000))),
            fetch="lastrowid")

    @staticmethod
    def _report_row(r: dict[str, Any]) -> dict[str, Any]:
        stats = json.loads(r["stats"]) if isinstance(r["stats"], (str, bytes)) else r["stats"]
        return {"day": str(r["day"]), "created_at": dt_ms(r["created_at"]), "stats": stats,
                "markdown": r.get("markdown")}

    async def get_report(self, day: str) -> dict[str, Any] | None:
        r = await self._run("SELECT * FROM ai_daily_report WHERE day = %s", (day,), fetch="one")
        return self._report_row(r) if r else None

    async def list_reports(self, limit: int) -> list[dict[str, Any]]:
        rows = await self._run(
            "SELECT day, stats, created_at FROM ai_daily_report ORDER BY day DESC LIMIT %s", (limit,))
        out = []
        for r in rows:
            row = self._report_row({**r, "markdown": None})
            st = row["stats"] or {}
            out.append({"day": row["day"], "created_at": row["created_at"], "llm": st.get("llm"),
                        "nodes": len(st.get("nodes") or []), "tasks": (st.get("totals") or {}).get("tasks")})
        return out

    async def close(self) -> None:
        await self._drop()



