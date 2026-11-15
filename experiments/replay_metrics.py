#!/usr/bin/env python3
"""按 Manager 的格式往 Redis 写合成样本（node:{id}:ts、stream:metrics）和事件，用于 AI 服务本机联调。

python experiments/replay_metrics.py --redis 127.0.0.1:6379 --password x --nodes 3 --seconds 400 \
    --fault w1:cpu:300:60 --event w2:evict:350
fault 格式 节点:指标(cpu|mem|runq|timeout):开始秒:持续秒；--warm N 先把前 N 秒直接写进 node:{id}:ts（不实时等待）。
"""

import argparse
import json
import random
import time

import redis


def sample(node, ts, faults, t, rnd):
    cpu, mem, runq, to = 0.03 + rnd.uniform(-0.02, 0.02), 0.8 + rnd.uniform(-0.01, 0.01), 80 + rnd.uniform(-20, 20), 0.0
    for fn, metric, start, dur in faults:
        if fn == node and start <= t < start + dur:
            if metric == "cpu":
                cpu = 0.98
            elif metric == "mem":
                mem = 0.04
            elif metric == "runq":
                runq = 22000
            elif metric == "timeout":
                to = 0.5
    return {"node_id": node, "seq": str(t), "ts_ms": str(ts), "cpu_busy": cpu, "mem_avail_ratio": mem,
            "disk_util": 0.01, "runq_p50_us": 20.0, "runq_p99_us": runq, "runq_drop_rate": 0.0,
            "nics": [{"name": "eth0", "rtt_ms": 1.0 + rnd.uniform(0, 0.3), "timeout_rate": to, "retrans_rate": 0.0}]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--redis", default="127.0.0.1:6379")
    ap.add_argument("--password", default="")
    ap.add_argument("--prefix", default="")
    ap.add_argument("--nodes", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=400)
    ap.add_argument("--warm", type=int, default=0)
    ap.add_argument("--fault", action="append", default=[])
    ap.add_argument("--event", action="append", default=[])
    a = ap.parse_args()
    host, _, port = a.redis.rpartition(":")
    r = redis.Redis(host=host, port=int(port), password=a.password or None, decode_responses=True)
    k = lambda s: a.prefix + s
    nodes = [f"w{i + 1}" for i in range(a.nodes)]
    faults = [(f.split(":")[0], f.split(":")[1], int(f.split(":")[2]), int(f.split(":")[3])) for f in a.fault]
    events = {int(e.split(":")[2]): (e.split(":")[0], e.split(":")[1]) for e in a.event}
    rnd = random.Random(7)
    t0 = int(time.time() * 1000) - a.warm * 1000
    for t in range(a.seconds):
        ts = t0 + t * 1000
        batch = [sample(n, ts, faults, t, rnd) for n in nodes]
        p = r.pipeline(transaction=False)
        for s in batch:
            p.zadd(k(f"node:{s['node_id']}:ts"), {json.dumps(s, separators=(",", ":")): ts})
            p.hset(k(f"node:{s['node_id']}"), mapping={"state": "normal", "score": "0.9", "cpu_busy": f"{s['cpu_busy']:.4f}"})
        if t >= a.warm:
            p.xadd(k("stream:metrics"), {"ts_ms": str(ts), "nodes": json.dumps(batch, separators=(",", ":"))},
                   maxlen=100000, approximate=True)
            if t in events:
                n, kind = events[t]
                p.xadd(k("stream:events"), {"node": n, "kind": kind, "ts_ms": str(ts),
                                            "detail": json.dumps({"from": "normal", "reason": "probe"})})
        p.execute()
        if t >= a.warm:
            time.sleep(max(0.0, (ts + 1000) / 1000 - time.time()))
    print("done", flush=True)


if __name__ == "__main__":
    main()
