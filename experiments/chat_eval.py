#!/usr/bin/env python3
"""M9 运维问答评估：20 个问题，标准答案由脚本当场从 Redis/MySQL 算出（不经过问答工具），
对 POST /api/ai/chat 的回答自动判分，统计正确率、平均工具调用次数、平均耗时。

在 Manager 机上用 AI 镜像跑（host 网络，环境变量与 compose 里的 ai 服务相同）：
  docker run --rm --network host --env-file <(…LM_REDIS_PASSWORD、LM_MYSQL_PASSWORD…) \
      -v $PWD/experiments:/exp:ro lm-ai python /exp/chat_eval.py --url http://127.0.0.1:8000 [--out r.jsonl]
实时指标在提问前后各取一次标准答案，回答与其中任一个在容差内即算对。
"""

import argparse
import asyncio
import json
import math
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, "/app")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ai"))

from lmai import config as config_mod  # noqa: E402
from lmai.store import MySQL, Redis  # noqa: E402

STATE_WORDS = {"normal": ["normal", "正常"], "slow_start": ["slow_start", "慢启动"],
               "evicted": ["evicted", "剔除"], "rescue": ["rescue", "急救"]}
REASON_WORDS = {"lost": ["lost", "上报中断", "失联", "中断"], "probe": ["probe", "探测"],
                "cpu": ["cpu", "CPU"], "mem": ["mem", "内存"], "runq": ["runq", "调度延迟"],
                "net": ["net", "网卡", "网络"], "gray": ["gray", "灰色"]}
SEV_WORDS = {"high": ["high", "高"], "medium": ["medium", "中"], "low": ["low", "低"]}


def numbers(text):
    text = re.sub(r"(?<=\d),(?=\d{3}\b)", "", text)
    return [float(x) for x in re.findall(r"(?<![\w.])\d+(?:\.\d+)?", text)]


def num_ok(text, truth, tol_abs=None, tol_rel=None, ratio=False):
    """回答里有一个数在容差内即可；ratio=True 时也接受百分数写法。"""
    if truth is None:
        return False
    for x in numbers(text):
        for v in ([x, x / 100] if ratio else [x]):
            if tol_abs is not None and abs(v - truth) <= tol_abs:
                return True
            if tol_rel is not None and abs(v - truth) <= tol_rel * max(abs(truth), 1e-9):
                return True
            if tol_abs is None and tol_rel is None and abs(v - truth) < 1e-9:
                return True
    return False


def words_ok(text, words):
    return any(w.lower() in text.lower() for w in words)


def f(v):
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (TypeError, ValueError):
        return None


class Truth:
    """从 Redis/MySQL 直接算标准答案。"""

    def __init__(self, cfg):
        self.r = Redis(cfg)
        self.m = MySQL(cfg)

    async def snapshot(self):
        nodes = await self.r.node_ids()
        hashes = await self.r.node_hashes(nodes)
        now = int(time.time() * 1000)
        online = [n for n, h in hashes.items() if now - (f(h.get("last_seen_ms")) or 0) <= 3000]
        return {"nodes": nodes, "hash": hashes, "online": online, "now": now}

    async def series(self, node, minutes, key):
        now = int(time.time() * 1000)
        vals = []
        for m in await self.r.node_ts(node, now - minutes * 60000):
            s = json.loads(m)
            if key in s or key in ("cpu_busy", "mem_avail_ratio", "disk_util"):
                vals.append(float(s.get(key, 0)))
        return vals

    async def tasks(self, hours):
        now = int(time.time() * 1000)
        rows = await self.m.task_rows(None, now - int(hours * 3600000), now + 1)
        ms = sorted(float(r["exec_ms"]) for r in rows if r.get("exec_ms") is not None)
        p99 = ms[max(0, math.ceil(0.99 * len(ms)) - 1)] if ms else None
        return {"tasks": len(rows), "timeouts": sum(1 for r in rows if r["state"] == "timeout"), "p99": p99}

    async def events(self, node, hours, kind=None):
        now = int(time.time() * 1000)
        return await self.m.node_events(node, now - int(hours * 3600000), now + 1, 1000,
                                        (kind,) if kind else ())

    async def last_diag(self, node, hours):
        rows = await self.m.list_diagnoses(node, None, hours, 1)
        return rows[0] if rows else None


def build_questions(snap):
    nodes = snap["nodes"]
    a = nodes[0]
    b = nodes[1] if len(nodes) > 1 else nodes[0]
    qs = []

    def q(text, check):
        qs.append((text, check))

    q("一共有几台节点？", lambda s, t, ans: num_ok(ans, len(s["nodes"])))
    q("现在有几台节点在线？", lambda s, t, ans: num_ok(ans, len(s["online"])))
    q("现在哪台节点的选中概率最高？",
      lambda s, t, ans: max(s["nodes"], key=lambda n: f(s["hash"][n].get("prob")) or 0) in ans)
    q("哪台节点的总分最低？", lambda s, t, ans: min(s["nodes"], key=lambda n: f(s["hash"][n].get("score")) or 0) in ans)
    q(f"{a} 现在的 CPU 忙碌度是多少？", lambda s, t, ans: num_ok(ans, f(s["hash"][a].get("cpu_busy")), 0.08, ratio=True))
    q(f"{b} 当前是什么状态？", lambda s, t, ans: words_ok(ans, STATE_WORDS.get(s["hash"][b].get("state"), ["?"])))
    q(f"{a} 的超时系数 f_timeout 是多少？", lambda s, t, ans: num_ok(ans, f(s["hash"][a].get("f_timeout")), 0.02))
    q(f"{b} 的 CPU 分项分数是多少？", lambda s, t, ans: num_ok(ans, f(s["hash"][b].get("s_cpu")), 0.05))
    q(f"{a} 的网络分来自哪块网卡？", lambda s, t, ans: (s["hash"][a].get("best_nic") or "无") in ans)
    q(f"最近 24 小时 {a} 被剔除了几次？", lambda s, t, ans: num_ok(ans, t["evict24_a"]))
    q(f"{a} 最近一次 AI 诊断的严重度是什么？（最近 24 小时）",
      lambda s, t, ans: words_ok(ans, SEV_WORDS.get((t["diag_a"] or {}).get("severity"), ["没有", "无"])))
    q("最近 1 小时一共分配了多少个任务？", lambda s, t, ans: num_ok(ans, t["tasks1h"]["tasks"], tol_rel=0.05))
    q("最近 1 小时任务完成时间的 P99 是多少毫秒？",
      lambda s, t, ans: num_ok(ans, t["tasks1h"]["p99"], tol_rel=0.15) or (t["tasks1h"]["p99"] is None and words_ok(ans, ["没有", "无"])))
    q("最近 1 小时有多少个任务执行超时？", lambda s, t, ans: num_ok(ans, t["tasks1h"]["timeouts"]))
    q(f"{b} 最近 10 分钟 CPU 平均是多少？", lambda s, t, ans: num_ok(ans, t["cpu10_b"], 0.05, ratio=True))
    q(f"{a} 最近 10 分钟调度延迟 P99 最高到过多少微秒？",
      lambda s, t, ans: num_ok(ans, t["runq10_a"], tol_rel=0.2) or (t["runq10_a"] is None and words_ok(ans, ["没有", "未上报", "无"])))
    q(f"{b} 最近 10 分钟可用内存比例最低是多少？", lambda s, t, ans: num_ok(ans, t["mem10_b"], 0.03, ratio=True))
    q(f"{a} 的负载预测因子 f_pred 现在是多少？", lambda s, t, ans: num_ok(ans, f(s["hash"][a].get("f_pred", "1")), 0.06))
    q(f"{b} 最近一次被剔除是什么原因？（看最近 7 天）",
      lambda s, t, ans: words_ok(ans, REASON_WORDS.get(t["evict_reason_b"], ["没有", "无"])))
    q("现在哪些节点处于剔除状态？",
      lambda s, t, ans: all(n in ans for n in s["nodes"] if s["hash"][n].get("state") == "evicted")
      and (any(s["hash"][n].get("state") == "evicted" for n in s["nodes"]) or words_ok(ans, ["没有", "无"])))
    return qs, a, b


async def truth_extra(tr, a, b):
    ev_b = await tr.events(b, 168, "evict")
    runq = await tr.series(a, 10, "runq_p99_us")
    mem = await tr.series(b, 10, "mem_avail_ratio")
    cpu = await tr.series(b, 10, "cpu_busy")
    return {"evict24_a": len(await tr.events(a, 24, "evict")), "diag_a": await tr.last_diag(a, 24),
            "tasks1h": await tr.tasks(1), "cpu10_b": sum(cpu) / len(cpu) if cpu else None,
            "runq10_a": max(runq) if runq else None, "mem10_b": min(mem) if mem else None,
            "evict_reason_b": (ev_b[0]["detail"].get("reason") if ev_b else None)}


def ask(url, question, token, timeout=180):
    body = json.dumps({"messages": [{"role": "user", "content": question}]}).encode()
    req = urllib.request.Request(url.rstrip("/") + "/api/ai/chat", data=body, method="POST",
                                 headers={"Content-Type": "application/json",
                                          **({"Authorization": f"Bearer {token}"} if token else {})})
    t0 = time.time()
    text, tools, done, err, ev = [], [], {}, None, None
    first = None
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        for raw in resp:
            line = raw.decode().rstrip("\n")
            if line.startswith("event: "):
                ev = line[7:]
            elif line.startswith("data: "):
                data = json.loads(line[6:])
                if first is None:
                    first = time.time() - t0
                if ev == "delta":
                    text.append(data["text"])
                elif ev == "tool_call":
                    tools.append(f"{data['name']}({json.dumps(data['args'], ensure_ascii=False)})")
                elif ev == "done":
                    done = data
                elif ev == "error":
                    err = data["reason"]
    return {"answer": "".join(text), "tools": tools, "done": done, "error": err,
            "ms": int((time.time() - t0) * 1000), "first_event_ms": int((first or 0) * 1000)}


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8000")
    ap.add_argument("--config", default=os.environ.get("LM_AI_CONFIG", "/etc/lm/ai.yaml"))
    ap.add_argument("--token", default=os.environ.get("LM_API_TOKEN", ""))
    ap.add_argument("--out")
    ap.add_argument("--only", type=int, nargs="*", help="只跑这些题号（从 1 开始）")
    a = ap.parse_args()
    tr = Truth(config_mod.load(a.config))
    snap0 = await tr.snapshot()
    if not snap0["nodes"]:
        print("没有节点", file=sys.stderr)
        return 1
    qs, na, nb = build_questions(snap0)
    out = open(a.out, "w") if a.out else None
    ok_n, tool_n, ms_all, asked = 0, 0, [], 0
    for i, (text, check) in enumerate(qs, 1):
        if a.only and i not in a.only:
            continue
        before = (await tr.snapshot(), await truth_extra(tr, na, nb))
        res = await asyncio.to_thread(ask, a.url, text, a.token)
        after = (await tr.snapshot(), await truth_extra(tr, na, nb))
        good = not res["error"] and any(check(s, t, res["answer"]) for s, t in (before, after))
        asked += 1
        ok_n += good
        tool_n += res["done"].get("tool_calls", 0)
        ms_all.append(res["ms"])
        mark = "✓" if good else "✗"
        print(f"{i:>2} {mark} {text}\n     工具：{'、'.join(res['tools']) or '无'}；{res['ms']} ms"
              f"{'；错误 ' + res['error'] if res['error'] else ''}\n     回答：{res['answer'][:160].replace(chr(10), ' ')}")
        if out:
            out.write(json.dumps({"i": i, "q": text, "ok": good, **res}, ensure_ascii=False, default=str) + "\n")
    print(f"\n正确 {ok_n}/{asked} = {ok_n / max(asked, 1) * 100:.0f}%；平均工具调用 {tool_n / max(asked, 1):.2f} 次；"
          f"平均耗时 {sum(ms_all) / max(len(ms_all), 1) / 1000:.1f} 秒")
    await tr.m.close()
    await tr.r.r.aclose()
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
