"""测试用的假 Redis / MySQL（只实现 M9 工具、日报用到的方法）。"""

import json


class FakeRedis:
    def __init__(self, hashes=None, ts=None, preds=None, events=None):
        self.hashes = hashes or {}
        self.ts = ts or {}            # node → [sample dict]
        self.preds = preds or {}
        self.events = events or []
        self.settings, self.keys, self.tokens = {}, {}, 0
        self.set_preds_calls = []

    async def ping(self):
        return True

    async def node_ids(self):
        return sorted(self.hashes)

    async def node_hashes(self, nodes):
        return {n: dict(self.hashes[n]) for n in nodes if n in self.hashes}

    async def node_ts(self, node, since_ms):
        return [json.dumps(s) for s in self.ts.get(node, []) if int(s["ts_ms"]) >= since_ms]

    async def get_preds(self):
        return dict(self.preds)

    async def set_preds(self, preds, ttl_s):
        self.set_preds_calls.append((preds, ttl_s))
        self.preds = preds

    async def events_since(self, since_ms, node=None, limit=100, scan_max=5000):
        return [e for e in self.events if e["ts_ms"] >= since_ms and (not node or e["node"] == node)][:limit]

    # LLM 设置与 token 计数
    async def get_selection(self):
        from lmai.llm import Selection
        v = self.settings.get("vendor", "")
        return Selection(v, self.settings.get("model", ""), self.keys.get(v, ""))

    async def used_today(self):
        return self.tokens

    async def add(self, n):
        self.tokens += n


class FakeMySQL:
    def __init__(self, nodes=(), m5=(), events=(), tasks=(), diags=(), down=False):
        self.nodes = list(nodes)
        self.m5 = list(m5)
        self.events = list(events)
        self.tasks = list(tasks)
        self.diags = list(diags)
        self.down = down
        self.reports = {}

    def _chk(self):
        if self.down:
            raise ConnectionError("mysql down")

    async def ping(self):
        return not self.down

    async def node_ids(self):
        self._chk()
        return list(self.nodes)

    async def metrics_5m(self, node, since_ms, until_ms):
        self._chk()
        return [dict(r) for r in self.m5 if (not node or r["node_id"] == node) and since_ms <= r["ts_ms"] < until_ms]

    async def node_events(self, node, since_ms, until_ms, limit=100, kinds=()):
        self._chk()
        out = [e for e in self.events if (not node or e["node"] == node) and since_ms <= e["ts_ms"] < until_ms
               and (not kinds or e["kind"] in kinds)]
        return sorted(out, key=lambda e: -e["ts_ms"])[:limit]

    async def event_counts(self, since_ms, until_ms, kinds):
        self._chk()
        c = {}
        for e in self.events:
            if since_ms <= e["ts_ms"] < until_ms and e["kind"] in kinds:
                c[(e["node"], e["kind"])] = c.get((e["node"], e["kind"]), 0) + 1
        return [{"node_id": n, "kind": k, "n": v} for (n, k), v in c.items()]

    async def task_rows(self, node, since_ms, until_ms, limit=500000):
        self._chk()
        return [dict(t) for t in self.tasks if (not node or t["node_id"] == node) and since_ms <= t["t_alloc"] < until_ms]

    async def list_diagnoses(self, node, severity, hours, limit):
        self._chk()
        return [d for d in self.diags if not node or d["node"] == node][:limit]

    async def get_diagnosis(self, did):
        self._chk()
        for d in self.diags:
            if d["id"] == did:
                return {**d, "report": d.get("report", {"causes": []})}
        return None

    async def diagnoses_between(self, node, since_ms, until_ms, limit=200):
        self._chk()
        return [d for d in self.diags if since_ms <= d["ts"] < until_ms and (not node or d["node"] == node)][:limit]

    async def put_report(self, day, stats, markdown):
        self._chk()
        self.reports[day] = {"day": day, "stats": stats, "markdown": markdown}

    async def get_report(self, day):
        self._chk()
        return self.reports.get(day)

    async def list_reports(self, limit):
        self._chk()
        return [{"day": d} for d in sorted(self.reports, reverse=True)][:limit]
