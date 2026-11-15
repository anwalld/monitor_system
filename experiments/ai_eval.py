#!/usr/bin/env python3
"""M8 异常检测评估：以故障注入时刻为真值，统计检出率、检出延迟、每小时误报（只用标准库）。

python3 experiments/ai_eval.py --url http://127.0.0.1:8088 --faults ai_faults.jsonl [--since-ms N] [--until-ms N]
- 检出：注入期间到结束后 grace 秒内，该节点出现预期指标族的异常；延迟 = 第一个匹配异常 − 注入开始。
- 误报：不落在任何注入窗口（开始到结束后 grace 秒）里的异常；按评估时长折算成每小时次数。
--idle 只统计误报（不需要注入记录）。
"""

import argparse
import json
import statistics
import time
import urllib.request

EXPECT = {
    "cpu": ["cpu"],
    "runq": ["runq_p99_us", "runq_drop_rate"],
    "mem": ["mem_avail"],
    "disk": ["disk"],
    "nic": ["rtt_ms", "timeout_rate", "retrans_rate"],
    "stop": ["evict", "rescue_start"],
}


def base(metric: str) -> str:
    return metric.rsplit(".", 1)[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8088")
    ap.add_argument("--token", default="")
    ap.add_argument("--faults", default="")
    ap.add_argument("--since-ms", type=int, default=0)
    ap.add_argument("--until-ms", type=int, default=0)
    ap.add_argument("--grace", type=float, default=30)
    ap.add_argument("--idle", action="store_true")
    ap.add_argument("--file", default="", help="从保存的异常列表 JSON 读取（AI 服务重启会清空内存里的列表）")
    a = ap.parse_args()

    faults = []
    if a.faults:
        with open(a.faults) as f:
            faults = [json.loads(l) for l in f if l.strip()]
    since = a.since_ms or (min(x["start_ms"] for x in faults) - 60000 if faults else 0)
    until = a.until_ms or int(time.time() * 1000)
    if a.file:
        with open(a.file) as f:
            raw = json.load(f)
    else:
        req = urllib.request.Request(f"{a.url}/api/ai/anomalies?since_ms={since}")
        if a.token:
            req.add_header("Authorization", f"Bearer {a.token}")
        raw = json.load(urllib.request.urlopen(req, timeout=10))["anomalies"]
    anomalies = [x for x in raw if since <= x["ts_ms"] <= until]

    g = int(a.grace * 1000)
    windows = [(x["node"], x["start_ms"], x["end_ms"] + g) for x in faults]
    rows, by_fault = [], {}
    for x in faults:
        exp = EXPECT[x["fault"]]
        hits = [y for y in anomalies if y["node"] == x["node"] and x["start_ms"] <= y["ts_ms"] <= x["end_ms"] + g
                and base(y["metric"]) in exp]
        first = min(hits, key=lambda y: y["ts_ms"]) if hits else None
        lat = (first["ts_ms"] - x["start_ms"]) / 1000 if first else None
        also = sorted({y["metric"] for y in anomalies if y["node"] == x["node"]
                       and x["start_ms"] <= y["ts_ms"] <= x["end_ms"] + g} - {first["metric"] if first else ""})
        rows.append((x["fault"], x["round"], first["metric"] if first else "-", lat, also))
        by_fault.setdefault(x["fault"], []).append(lat)

    fps = [y for y in anomalies if not any(n == y["node"] and s <= y["ts_ms"] <= e for n, s, e in windows)]
    hours = max(1e-9, (until - since) / 3.6e6)

    if rows:
        print("| 故障 | 轮 | 首个匹配异常 | 延迟 s | 同时出现 |")
        print("|---|---|---|---|---|")
        for f, r, m, lat, also in rows:
            print(f"| {f} | {r} | {m} | {'未检出' if lat is None else f'{lat:.1f}'} | {', '.join(also) or '-'} |")
        print()
        print("| 故障 | 检出 | 延迟中位数 s | 最大 s |")
        print("|---|---|---|---|")
        for f, lats in by_fault.items():
            ok = [x for x in lats if x is not None]
            med = f"{statistics.median(ok):.1f}" if ok else "-"
            mx = f"{max(ok):.1f}" if ok else "-"
            print(f"| {f} | {len(ok)}/{len(lats)} | {med} | {mx} |")
        tot = sum(1 for _, _, _, lat, _ in rows if lat is not None)
        print(f"\n检出率 {tot}/{len(rows)}")
    print(f"评估时长 {hours:.2f} 小时，误报 {len(fps)} 次，{len(fps) / hours:.2f} 次/小时")
    for y in fps:
        print(f"  误报 {time.strftime('%H:%M:%S', time.localtime(y['ts_ms'] / 1000))} {y['node']} {y['rule']} "
              f"{y['metric']} value={y.get('value')} median={y.get('median')} z={y.get('z')}")


if __name__ == "__main__":
    main()
