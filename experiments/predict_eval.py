#!/usr/bin/env python3
"""M9 负载预测离线评估：用 node:{id}:ts 最近一段时间的每秒样本回放 Holt 预测，与朴素基线（当前值当预测）比较
30 秒后的平均绝对误差（MAE）。与 AI 服务用同一份代码（ai/lmai/predict.py）。

在 AI 镜像里跑（自带 redis-py）：
  docker run --rm --network host -v $PWD/experiments:/exp:ro -e LM_REDIS_PASSWORD=… lm-ai \
      python /exp/predict_eval.py --redis 127.0.0.1:6379 [--minutes 60] [--grid]
--grid 扫描 alpha × beta，按总 MAE 排序；--file 读本地 JSONL（每行一个样本，含 node_id、ts_ms、cpu_busy）。
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, "/app")  # 镜像里 lmai 在 /app
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ai"))

from lmai.predict import Predictor  # noqa: E402


def load_redis(addr, password, minutes, prefix=""):
    import redis
    host, _, port = addr.rpartition(":")
    r = redis.Redis(host=host or addr, port=int(port or 6379), password=password or None, decode_responses=True)
    since = int(time.time() * 1000) - minutes * 60000
    out = {}
    for k in r.scan_iter(match=prefix + "node:*:ts", count=200):
        node = k[len(prefix) + 5:-3]
        rows = []
        for m in r.zrangebyscore(k, since, "+inf"):
            try:
                s = json.loads(m)
                rows.append((int(float(s.get("ts_ms") or 0)), float(s.get("cpu_busy") or 0.0)))
            except (ValueError, json.JSONDecodeError):
                pass
        if rows:
            out[node] = sorted(rows)
    return out


def load_file(path):
    out = {}
    with open(path) as f:
        for line in f:
            s = json.loads(line)
            out.setdefault(s["node_id"], []).append((int(float(s["ts_ms"])), float(s.get("cpu_busy") or 0)))
    return {n: sorted(v) for n, v in out.items()}


def replay(data, alpha, beta, horizon_s=30, interval_s=5, min_samples=30, phi=0.9):
    """按样本时间回放：每 interval_s 秒 tick 一次（与线上一致），最后返回 view。"""
    p = Predictor(horizon_s=horizon_s, alpha=alpha, beta=beta, min_samples=min_samples, eval_window_s=10 ** 9,
                  phi=phi)
    events = sorted((ts, n, x) for n, rows in data.items() for ts, x in rows)
    if not events:
        return p.view(0)
    next_tick = events[0][0] + interval_s * 1000
    for ts, n, x in events:
        while ts >= next_tick:
            p.tick(next_tick)
            next_tick += interval_s * 1000
        p.observe(n, ts, x)
    return p.view(events[-1][0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--redis", default="127.0.0.1:6379")
    ap.add_argument("--password", default=os.environ.get("LM_REDIS_PASSWORD", ""))
    ap.add_argument("--prefix", default="")
    ap.add_argument("--minutes", type=int, default=60)
    ap.add_argument("--file")
    ap.add_argument("--alpha", type=float, default=0.3)
    ap.add_argument("--beta", type=float, default=0.05)
    ap.add_argument("--phi", type=float, default=0.9, help="阻尼趋势系数，1.0 为普通 Holt")
    ap.add_argument("--horizon", type=float, default=30)
    ap.add_argument("--grid", action="store_true")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    a = ap.parse_args()
    data = load_file(a.file) if a.file else load_redis(a.redis, a.password, a.minutes, a.prefix)
    if not data:
        print("没有样本", file=sys.stderr)
        return 1
    span = {n: (rows[-1][0] - rows[0][0]) / 1000 for n, rows in data.items()}
    v = replay(data, a.alpha, a.beta, a.horizon, phi=a.phi)
    if a.json:
        print(json.dumps(v, ensure_ascii=False))
    else:
        print(f"Holt α={a.alpha} β={a.beta} φ={a.phi}，预测 {a.horizon:.0f} 秒后的 CPU 忙碌度")
        print(f"{'节点':<18}{'样本':>7}{'时长 s':>9}{'点数':>7}{'MAE Holt':>11}{'MAE 基线':>11}{'改善':>8}")
        for n, s in v["nodes"].items():
            imp = (1 - s["mae_holt"] / s["mae_naive"]) * 100 if s["mae_naive"] else 0
            print(f"{n:<18}{len(data[n]):>7}{span[n]:>9.0f}{s['points']:>7}{s['mae_holt'] or 0:>11.4f}"
                  f"{s['mae_naive'] or 0:>11.4f}{imp:>7.1f}%")
        t = v["total"]
        imp = (1 - t["mae_holt"] / t["mae_naive"]) * 100 if t["mae_naive"] else 0
        print(f"{'合计':<18}{'':>7}{'':>9}{t['points']:>7}{t['mae_holt'] or 0:>11.4f}{t['mae_naive'] or 0:>11.4f}{imp:>7.1f}%")
    if a.grid:
        rows = []
        for al in (0.1, 0.2, 0.3, 0.5):
            for be in (0.0, 0.02, 0.1):
                for ph in (0.8, 0.9, 1.0):
                    if be == 0 and ph != 1.0:
                        continue
                    t = replay(data, al, be, a.horizon, phi=ph)["total"]
                    rows.append((t["mae_holt"] or 9, al, be, ph, t["mae_naive"] or 0))
        rows.sort()
        print("\nα × β × φ 扫描（按 MAE 升序，前 8）：")
        for m, al, be, ph, nv in rows[:8]:
            print(f"  α={al:<4} β={be:<5} φ={ph:<4} MAE {m:.4f}（基线 {nv:.4f}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
