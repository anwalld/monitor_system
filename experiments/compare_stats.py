#!/usr/bin/env python3
"""M10 对照实验统计：读 compare.py 的输出目录，输出每次运行的指标和按组汇总（Markdown + JSON）。

  python3 experiments/compare_stats.py experiments/results/m10 [--md out.md] [--json out.json]

口径（见 openspec comparison-experiment）：
- 完成时间：成功任务从 Allocate 到 Submit 返回的毫秒数；P50、P99、最大值；吞吐 = 成功数 / 运行秒数。
- 干扰期 P99：CPU 干扰开始后、持续时间内发起的成功任务的 P99。
- 反应时间：CPU 干扰节点在干扰前 30 秒的分配占比为基线 b；5 秒滑动窗口（步长 0.5 秒）占比首次 ≤ b/2 时，
  窗口终点减干扰开始时刻；干扰持续时间内没降到一半记“未降”；b < 0.1 或窗口内分配数 < 10 不计。
- 组间比较：各组相对第一组（默认 rr）的差值、Cohen's d、Welch t 检验 p 值（自由度用 Welch–Satterthwaite，
  p 值用正态近似，样本小时偏乐观）。
只用标准库。
"""
import argparse
import csv
import json
import math
import os
import statistics as st


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    i = min(len(xs) - 1, int(p * (len(xs) - 1) + 0.5))
    return xs[i]


def load_run(d):
    rows = []
    with open(os.path.join(d, "bench.csv")) as f:
        for r in csv.DictReader(f):
            r["start_ms"] = int(r["start_ms"])
            r["latency_ms"] = int(r["latency_ms"])
            rows.append(r)
    with open(os.path.join(d, "faults.json")) as f:
        faults = json.load(f)
    return rows, faults


def reaction(rows, addr, t_start, dur, win=5.0, step=0.5, base_s=30.0):
    """返回 (反应秒数或 None, 状态, 基线占比)。状态：ok / 未降 / 不计。"""
    allocs = [(r["start_ms"] / 1000.0, r["worker_addr"]) for r in rows if r["worker_addr"]]
    pre = [a for t, a in allocs if t_start - base_s <= t < t_start]
    if len(pre) < 10:
        return None, "不计", None
    b = sum(1 for a in pre if a == addr) / len(pre)
    if b < 0.1:
        return None, "不计", b
    w = t_start + step
    while w <= t_start + dur:
        win_allocs = [a for t, a in allocs if w - win <= t < w]
        if len(win_allocs) >= 10 and sum(1 for a in win_allocs if a == addr) / len(win_allocs) <= b / 2:
            return round(w - t_start, 1), "ok", b
        w += step
    return None, "未降", b


def share(rows, addr, t0, t1):
    xs = [r["worker_addr"] for r in rows if r["worker_addr"] and t0 <= r["start_ms"] / 1000.0 < t1]
    return sum(1 for a in xs if a == addr) / len(xs) if xs else None


def run_metrics(d, plan):
    rows, fj = load_run(d)
    ok = [r for r in rows if r["stage"] == "done"]
    lat = [r["latency_ms"] for r in ok]
    fails = {}
    for r in rows:
        if r["stage"] != "done":
            k = "%s/%s" % (r["stage"], r["code"])
            fails[k] = fails.get(k, 0) + 1
    m = {
        "tasks": len(rows), "ok": len(ok), "fail": len(rows) - len(ok), "fail_by": fails,
        "p50": pct(lat, 0.5), "p99": pct(lat, 0.99), "max": max(lat) if lat else None,
        "throughput": round(len(ok) / plan["duration"], 3),
        "kind_p99": {}, "kind_p50": {},
    }
    for k in sorted({r["kind"] for r in rows}):
        kl = [r["latency_ms"] for r in ok if r["kind"] == k]
        m["kind_p99"][k] = pct(kl, 0.99)
        m["kind_p50"][k] = pct(kl, 0.5)
    addr_of = {n["name"]: n["addr"] for n in plan["nodes"]}
    for f in fj["faults"]:
        if f["kind"] != "cpu":
            continue
        t = f.get("actual_start_s", f["start_s"])
        dur = f["dur_s"]
        addr = addr_of[f["node"]]
        fl = [r["latency_ms"] for r in ok if t <= r["start_ms"] / 1000.0 < t + dur]
        m["fault_node"] = f["node"]
        m["fault_p99"] = pct(fl, 0.99)
        m["fault_p50"] = pct(fl, 0.5)
        rt, status, b = reaction(rows, addr, t, dur)
        m["reaction_s"], m["reaction_status"], m["baseline_share"] = rt, status, b
        m["fault_share"] = share(rows, addr, t + 10, t + dur)  # 干扰开始 10 秒后到结束的占比
    return m


def mean_sd(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None, None, 0
    return st.mean(xs), (st.stdev(xs) if len(xs) > 1 else 0.0), len(xs)


def welch(a, b):
    """返回 (差值 b−a 的均值, Cohen's d, p 值)。"""
    a = [x for x in a if x is not None]
    b = [x for x in b if x is not None]
    if len(a) < 2 or len(b) < 2:
        return None, None, None
    ma, mb = st.mean(a), st.mean(b)
    va, vb = st.variance(a), st.variance(b)
    sp = math.sqrt(((len(a) - 1) * va + (len(b) - 1) * vb) / (len(a) + len(b) - 2))
    d = (mb - ma) / sp if sp > 0 else None
    se = math.sqrt(va / len(a) + vb / len(b))
    if se == 0:
        return mb - ma, d, None
    t = (mb - ma) / se
    p = math.erfc(abs(t) / math.sqrt(2))  # 正态近似的双侧 p
    return mb - ma, d, p


def fmt(x, nd=0):
    if x is None:
        return "—"
    return ("%." + str(nd) + "f") % x


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--md")
    ap.add_argument("--json")
    a = ap.parse_args()
    with open(os.path.join(a.dir, "plan.json")) as f:
        plan = json.load(f)
    groups = plan["groups"].split(",")
    runs = []
    for x in plan["plan"]:
        d = os.path.join(a.dir, "r%d-%s" % (x["round"], x["group"]))
        if not os.path.exists(os.path.join(d, "done")):
            continue
        m = run_metrics(d, plan)
        m.update(round=x["round"], group=x["group"], seed=x["seed"])
        runs.append(m)
    out = []
    out.append("## 每次运行\n")
    out.append("| 轮 | 组 | 任务 | 失败 | P50 ms | P99 ms | 最大 ms | 吞吐/s | 干扰节点 | 干扰期 P99 | 基线占比 | 干扰期占比 | 反应 s |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for m in sorted(runs, key=lambda m: (m["round"], groups.index(m["group"]))):
        out.append("| %d | %s | %d | %d | %s | %s | %s | %.2f | %s | %s | %s | %s | %s |" % (
            m["round"], m["group"], m["tasks"], m["fail"], fmt(m["p50"]), fmt(m["p99"]), fmt(m["max"]),
            m["throughput"], m.get("fault_node", "—"), fmt(m.get("fault_p99")), fmt(m.get("baseline_share"), 2),
            fmt(m.get("fault_share"), 2),
            fmt(m.get("reaction_s"), 1) if m.get("reaction_status") == "ok" else m.get("reaction_status", "—")))
    out.append("\n## 按组汇总（均值 ± 标准差，n = 轮数）\n")
    out.append("| 组 | n | P50 ms | P99 ms | 最大 ms | 干扰期 P99 ms | 吞吐/s | 失败 | 反应 s（中位，降到一半的次数/计入次数） |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    summary = {}
    for g in groups:
        gr = [m for m in runs if m["group"] == g]
        if not gr:
            continue
        row = {}
        for k in ("p50", "p99", "max", "fault_p99", "throughput", "fail"):
            row[k] = mean_sd([m.get(k) for m in gr])
        rts = [m["reaction_s"] for m in gr if m.get("reaction_status") == "ok"]
        counted = [m for m in gr if m.get("reaction_status") in ("ok", "未降")]
        row["reaction_median"] = st.median(rts) if rts else None
        row["reaction_ok"] = len(rts)
        row["reaction_counted"] = len(counted)
        summary[g] = row

        def ms(k, nd=0):
            mu, sd, _ = row[k]
            return "%s ± %s" % (fmt(mu, nd), fmt(sd, nd))
        out.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s（%d/%d） |" % (
            g, len(gr), ms("p50"), ms("p99"), ms("max"), ms("fault_p99"), ms("throughput", 2), ms("fail", 1),
            fmt(row["reaction_median"], 1), row["reaction_ok"], row["reaction_counted"]))
    base = groups[0]
    out.append("\n## 相对 %s 的差（负数表示更快）\n" % base)
    out.append("| 组 | 指标 | 差值 ms | Cohen's d | p（Welch，正态近似） |")
    out.append("|---|---|---|---|---|")
    comp = {}
    bruns = [m for m in runs if m["group"] == base]
    for g in groups[1:]:
        gr = [m for m in runs if m["group"] == g]
        for k in ("p50", "p99", "fault_p99"):
            diff, d, p = welch([m.get(k) for m in bruns], [m.get(k) for m in gr])
            comp["%s.%s" % (g, k)] = {"diff": diff, "d": d, "p": p}
            out.append("| %s | %s | %s | %s | %s |" % (g, k, fmt(diff), fmt(d, 2), fmt(p, 4)))
    # score 与 score_nopred 的直接对比
    if "score" in groups and "score_nopred" in groups:
        out.append("\n## score 相对 score_nopred（预测的作用）\n")
        out.append("| 指标 | 差值 ms | Cohen's d | p |")
        out.append("|---|---|---|---|")
        a_ = [m for m in runs if m["group"] == "score_nopred"]
        b_ = [m for m in runs if m["group"] == "score"]
        for k in ("p50", "p99", "fault_p99"):
            diff, d, p = welch([m.get(k) for m in a_], [m.get(k) for m in b_])
            comp["score_vs_nopred.%s" % k] = {"diff": diff, "d": d, "p": p}
            out.append("| %s | %s | %s | %s |" % (k, fmt(diff), fmt(d, 2), fmt(p, 4)))
    out.append("\n## 按任务类型的 P99（各轮均值，ms）\n")
    kinds = sorted({k for m in runs for k in m["kind_p99"]})
    out.append("| 组 | " + " | ".join(kinds) + " |")
    out.append("|---|" + "---|" * len(kinds))
    for g in groups:
        gr = [m for m in runs if m["group"] == g]
        if gr:
            out.append("| %s | " % g + " | ".join(fmt(mean_sd([m["kind_p99"].get(k) for m in gr])[0]) for k in kinds)
                       + " |")
    text = "\n".join(out) + "\n"
    print(text)
    if a.md:
        with open(a.md, "w") as f:
            f.write(text)
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"runs": runs, "summary": summary, "compare": comp}, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
