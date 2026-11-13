#!/usr/bin/env python3
"""对比本系统的调度延迟分布与 bcc runqlat。

用法：
  compare_runqlat.py runqlat.txt ours.json     # runqlat-bpfcc 30 1 的输出；runq_compare 的 JSON
  compare_runqlat.py --self-test

方法（见 openspec/changes/m3-ebpf-runqlat/specs/worker-runq-latency/spec.md）：
  把 ours 的每个对数线性桶按下界归入 runqlat 的同一个 2 的幂区间，两边各自归一化，
  计算总变差距离（各区间比例差绝对值之和的一半），并在 2 的幂区间内线性插值估算 P50、P99。
  判定：总变差距离 <= 0.1，P50、P99 相差 <= 2 倍。
"""
import json
import re
import sys

LINE = re.compile(r"^\s*(\d+)\s*->\s*(\d+)\s*:\s*(\d+)")
TV_MAX = 0.10
RATIO_MAX = 2.0


def parse_runqlat(text):
    """返回 [(low, high, count)]，high 为闭区间上界。"""
    bins = []
    for line in text.splitlines():
        m = LINE.match(line)
        if m:
            bins.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    if not bins:
        raise ValueError("没有在 runqlat 输出中找到直方图行")
    return bins


def rebin(ours, bins):
    """ours: {"buckets": [[low, width, count], ...]} → 与 bins 对齐的计数列表。"""
    counts = [0] * len(bins)
    for low, _width, cnt in ours["buckets"]:
        for i, (lo, hi, _c) in enumerate(bins):
            if lo <= low <= hi:
                counts[i] += cnt
                break
        else:
            # 超出 runqlat 打印范围：归入最近的端点区间
            counts[0 if low < bins[0][0] else -1] += cnt
    return counts


def percentile(bins, counts, p):
    n = sum(counts)
    if n == 0:
        return 0.0
    rank = p * n
    cum = 0
    for (lo, hi, _c), c in zip(bins, counts):
        if c and cum + c >= rank:
            return lo + (rank - cum) / c * (hi + 1 - lo)
        cum += c
    lo, hi, _ = bins[-1]
    return float(hi + 1)


def compare(runqlat_text, ours):
    bins = parse_runqlat(runqlat_text)
    theirs = [c for _, _, c in bins]
    mine = rebin(ours, bins)
    nt, nm = sum(theirs), sum(mine)
    if nt == 0 or nm == 0:
        raise ValueError(f"样本为空：runqlat {nt}，ours {nm}")
    tv = 0.5 * sum(abs(a / nt - b / nm) for a, b in zip(theirs, mine))
    res = {"runqlat_samples": nt, "ours_samples": nm, "tv_distance": round(tv, 4)}
    ok = tv <= TV_MAX
    for name, p in (("p50", 0.5), ("p99", 0.99)):
        a, b = percentile(bins, theirs, p), percentile(bins, mine, p)
        ratio = max(a, b) / max(min(a, b), 1e-9)
        res[f"runqlat_{name}_us"] = round(a, 1)
        res[f"ours_{name}_us"] = round(b, 1)
        res[f"{name}_ratio"] = round(ratio, 3)
        ok = ok and ratio <= RATIO_MAX
    res["result"] = "PASS" if ok else "FAIL"
    return res


SAMPLE_RUNQLAT = """Tracing run queue latency... Hit Ctrl-C to end.

     usecs               : count     distribution
         0 -> 1          : 100      |****                |
         2 -> 3          : 300      |************        |
         4 -> 7          : 500      |********************|
         8 -> 15         : 80       |***                 |
        16 -> 31         : 20       |                    |
"""


def self_test():
    # 与 runqlat 完全相同的分布 → 距离 0
    ours = {"buckets": [[0, 1, 50], [1, 1, 50], [2, 1, 300], [4, 1, 500], [8, 1, 80], [16, 2, 20]]}
    r = compare(SAMPLE_RUNQLAT, ours)
    assert r["tv_distance"] == 0 and r["result"] == "PASS", r
    # 把一半样本挪到 16-31 → 距离明显变大，判 FAIL
    ours_bad = {"buckets": [[2, 1, 150], [4, 1, 250], [16, 2, 600]]}
    r = compare(SAMPLE_RUNQLAT, ours_bad)
    assert r["tv_distance"] > TV_MAX and r["result"] == "FAIL", r
    print("self-test PASS")


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        self_test()
        return 0
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1]) as f:
        text = f.read()
    with open(argv[2]) as f:
        ours = json.loads(f.read())
    res = compare(text, ours)
    print(json.dumps(res, ensure_ascii=False))
    return 0 if res["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
