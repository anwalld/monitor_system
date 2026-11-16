#!/usr/bin/env python3
"""M10 对照实验驱动：按轮交替跑各组，注入本地 CPU 负载（和可选的网卡劣化），保存明细。

只用标准库（Mac 自带的 python3 3.9 即可）。每次运行：
  1. 等 --gap 秒（ALLOCATED 任务 30 秒作废、上一组的干扰残留消失），再等所有节点都不在急救中（最多 120 秒）；
  2. 保存节点状态 nodes_before.json，启动 lm-bench submit（同一轮四组同一种子 → 同一任务序列）；
  3. 按本轮的干扰计划在指定时刻注入、撤销，实际时刻写 faults.json（相对 lm-bench 启动的秒数）。
第 r 轮的组顺序是基础顺序左移 r 位。Ctrl-C 时结束 lm-bench 与 stress、删除 netem。

节点写法（可重复 --node）：
  名称=Worker地址,ssh:<user@host>,<核数>[,<数据面网卡>]
  名称=Worker地址,docker:<容器名>,<核数>[,<数据面网卡>]
示例（M10 的三节点，见 README“对照实验”；Mac 上用 caffeinate 防止休眠打断）：
  caffeinate -i python3 experiments/compare.py --out experiments/results/m10-3node --token "$TOKEN" \\
    --node lm-worker-1=172.16.255.133:50052,ssh:ubuntu@172.16.255.133,4,ens192 \\
    --node lmtest-worker2=172.16.255.1:50052,docker:lmtest-worker2,2,eth1 \\
    --node lmtest-worker3=172.16.13.1:50052,docker:lmtest-worker3,2,eth0 \\
    --host-map 172.16.255.133:50052=172.16.13.1 --host-map 172.16.255.1:50052=host.docker.internal \\
    --host-map 172.16.13.1:50052=host.docker.internal
"""
import argparse
import json
import os
import random
import signal
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


class Node:
    def __init__(self, spec):
        name, rest = spec.split("=", 1)
        parts = rest.split(",")
        if len(parts) < 3:
            raise SystemExit("--node 格式：名称=地址,ssh:host|docker:容器,核数[,网卡]：" + spec)
        self.name = name
        self.addr = parts[0]
        kind, target = parts[1].split(":", 1)
        if kind not in ("ssh", "docker"):
            raise SystemExit("节点类型只能是 ssh 或 docker：" + spec)
        self.kind = kind
        self.target = target
        self.cores = int(parts[2])
        self.nic = parts[3] if len(parts) > 3 else ""

    def cmd(self, shell):
        """在节点上执行 shell 命令的 argv。"""
        if self.kind == "ssh":
            return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", self.target, shell]
        return ["docker", "exec", self.target, "sh", "-c", shell]

    def run(self, shell, check=False, timeout=30):
        r = subprocess.run(self.cmd(shell), stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           timeout=timeout)
        if check and r.returncode != 0:
            raise RuntimeError("%s: %s 失败：%s" % (self.name, shell, r.stderr.strip()))
        return r

    def sudo(self):
        return "sudo " if self.kind == "ssh" else ""


def plan_round(nodes, seed, duration, cpu_s, net_s, net_fault):
    """本轮的干扰计划：由种子决定，同一轮四组相同。"""
    rng = random.Random(seed)
    faults = []
    n = rng.choice(nodes)
    faults.append({"kind": "cpu", "node": n.name, "start_s": round(rng.uniform(0.3, 0.5) * duration, 1),
                   "dur_s": cpu_s})
    if net_fault:
        cand = [x for x in nodes if x.nic]
        if cand:
            m = rng.choice(cand)
            start = round(rng.uniform(0.2, 0.7) * duration, 1)
            faults.append({"kind": "net", "node": m.name, "start_s": start,
                           "dur_s": min(net_s, max(1.0, duration - start - 1))})
    return faults


class Injector:
    """注入与撤销；记录实际时刻。"""

    STRESS_TAG = "stress -c %d -t %ds"

    def __init__(self, nodes, netem):
        self.by_name = {n.name: n for n in nodes}
        self.netem = netem
        self.procs = []      # (fault, Popen)
        self.netem_on = []   # nodes

    def start(self, f, t0):
        n = self.by_name[f["node"]]
        f["actual_start_s"] = round(time.monotonic() - t0, 2)
        if f["kind"] == "cpu":
            tag = self.STRESS_TAG % (n.cores, int(f["dur_s"]))
            p = subprocess.Popen(n.cmd(tag), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            self.procs.append((f, p, n, tag))
        else:
            sh = n.sudo() + "tc qdisc replace dev %s root netem %s" % (n.nic, self.netem)
            if n.kind == "ssh":
                # 先设好自动撤销（不碰 ssh 走的网卡，但仍按约定留后路）
                n.run("sudo systemd-run --on-active=%ds --unit=lm-m10-netem-%d /usr/sbin/tc qdisc del dev %s root"
                      % (int(f["dur_s"]) + 60, int(time.time()), n.nic))
            n.run(sh, check=True)
            self.netem_on.append(n)

    def stop(self, f, t0):
        n = self.by_name[f["node"]]
        if f["kind"] == "cpu":
            for item in list(self.procs):
                if item[0] is f:
                    self._kill(item)
                    self.procs.remove(item)
        else:
            self._del_netem(n)
        f["actual_stop_s"] = round(time.monotonic() - t0, 2)

    def _kill(self, item):
        _, p, n, tag = item
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        n.run("pkill -f '[%s]%s'" % (tag[0], tag[1:]))  # [s]tress：不匹配 pkill 所在 shell 自己的命令行

    def _del_netem(self, n):
        if n in self.netem_on:
            n.run(n.sudo() + "tc qdisc del dev %s root" % n.nic)
            self.netem_on.remove(n)

    def cleanup(self):
        for item in list(self.procs):
            self._kill(item)
        self.procs.clear()
        for n in list(self.netem_on):
            self._del_netem(n)


def get_nodes(api):
    try:
        with urllib.request.urlopen(api.rstrip("/") + "/api/nodes", timeout=5) as r:
            return json.load(r)
    except Exception as e:  # noqa: BLE001 — 看板接口挂了不影响实验
        return {"error": str(e)}


def wait_ready(api, max_wait):
    deadline = time.monotonic() + max_wait
    while True:
        d = get_nodes(api)
        rescue = [n["id"] for n in d.get("nodes", []) if n.get("state") == "rescue"]
        if not rescue or time.monotonic() > deadline:
            return d, rescue
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="输出目录")
    ap.add_argument("--node", action="append", required=True)
    ap.add_argument("--manager", default="172.16.255.133:50051")
    ap.add_argument("--api", default="http://172.16.255.133", help="看板接口（读节点状态）")
    ap.add_argument("--bench", default=os.path.join(ROOT, "build", "experiments", "lm-bench"))
    ap.add_argument("--workload", default=os.path.join(HERE, "compare_workload.txt"))
    ap.add_argument("--host-map", action="append", default=[])
    ap.add_argument("--token", default=os.environ.get("LM_TASK_TOKEN", ""))
    ap.add_argument("--groups", default="rr,wrr,score_nopred,score")
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--first-round", type=int, default=0, help="从第几轮开始（中断后续跑）")
    ap.add_argument("--seed-base", type=int, default=1000)
    ap.add_argument("--rate", type=float, default=8)
    ap.add_argument("--duration", type=float, default=150)
    ap.add_argument("--timeout", type=int, default=30, help="单个任务超时秒数")
    ap.add_argument("--gap", type=float, default=35)
    ap.add_argument("--cpu-fault-s", type=float, default=60)
    ap.add_argument("--net-fault", action="store_true", help="另加网卡劣化（默认关，见 design.md）")
    ap.add_argument("--net-fault-s", type=float, default=45)
    ap.add_argument("--netem", default="delay 50ms loss 3%")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    nodes = [Node(s) for s in a.node]
    groups = a.groups.split(",")
    os.makedirs(a.out, exist_ok=True)
    runs = []
    for r in range(a.first_round, a.rounds):
        seed = a.seed_base + r
        faults = plan_round(nodes, seed, a.duration, a.cpu_fault_s, a.net_fault_s, a.net_fault)
        order = groups[r % len(groups):] + groups[:r % len(groups)]
        for g in order:
            runs.append({"round": r, "group": g, "seed": seed, "faults": faults})
    meta = {k: v for k, v in vars(a).items() if k != "token"}
    meta["nodes"] = [vars(n) for n in nodes]
    meta["plan"] = runs
    with open(os.path.join(a.out, "plan.json"), "w") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    for x in runs:
        print("r%d %-12s seed %d  %s" % (x["round"], x["group"], x["seed"], "; ".join(
            "%s@%s %.0fs+%.0fs" % (f["kind"], f["node"], f["start_s"], f["dur_s"]) for f in x["faults"])))
    print("共 %d 次运行，每次约 %.0f 秒" % (len(runs), a.duration + a.gap + 10))
    if a.dry_run:
        return

    if not os.access(a.bench, os.X_OK):
        raise SystemExit("找不到 lm-bench：" + a.bench)
    # Manager 是否支持对照策略
    chk = subprocess.run([a.bench, "allocate", "-m", a.manager, "-n", "4", "--policy", "rr", "-c", "m10-check"],
                         capture_output=True, text=True)
    if chk.returncode != 0:
        raise SystemExit("Manager 不支持 policy=rr 或没有可用节点：\n" + chk.stdout + chk.stderr)

    inj = Injector(nodes, a.netem)
    bench = {"p": None}

    def on_signal(signum, _frame):
        print("\n收到信号 %d，清理中…" % signum, flush=True)
        if bench["p"] is not None and bench["p"].poll() is None:
            bench["p"].terminate()
        inj.cleanup()
        sys.exit(130)

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    for i, x in enumerate(runs):
        d = os.path.join(a.out, "r%d-%s" % (x["round"], x["group"]))
        if os.path.exists(os.path.join(d, "done")):
            print("跳过已完成的", d)
            continue
        os.makedirs(d, exist_ok=True)
        time.sleep(a.gap)
        before, rescue = wait_ready(a.api, 120)
        with open(os.path.join(d, "nodes_before.json"), "w") as f:
            json.dump(before, f, ensure_ascii=False)
        faults = [dict(f) for f in x["faults"]]
        argv = [a.bench, "submit", "-m", a.manager, "-r", str(a.rate), "-d", str(a.duration), "-s", str(x["seed"]),
                "-c", "m10-" + x["group"], "--policy", x["group"], "--workload", a.workload,
                "--timeout", str(a.timeout), "--csv", os.path.join(d, "bench.csv")]
        for hm in a.host_map:
            argv += ["--host-map", hm]
        if a.token:
            argv += ["--token", a.token]
        print("[%d/%d] %s 开始 %s%s" % (i + 1, len(runs), d, time.strftime("%H:%M:%S"),
                                       "（仍有急救节点 %s）" % rescue if rescue else ""), flush=True)
        log = open(os.path.join(d, "bench.log"), "w")
        t0 = time.monotonic()
        wall0 = time.time()
        bench["p"] = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        events = []
        for f in faults:
            events.append((f["start_s"], "start", f))
            events.append((f["start_s"] + f["dur_s"], "stop", f))
        events.sort(key=lambda e: e[0])
        for at, what, f in events:
            while time.monotonic() - t0 < at and bench["p"].poll() is None:
                time.sleep(0.05)
            if bench["p"].poll() is not None and what == "start":
                continue
            (inj.start if what == "start" else inj.stop)(f, t0)
        rc = bench["p"].wait()
        inj.cleanup()
        log.close()
        with open(os.path.join(d, "faults.json"), "w") as f:
            json.dump({"wall_start": wall0, "rc": rc, "faults": faults}, f, ensure_ascii=False, indent=1)
        tail = open(os.path.join(d, "bench.log")).read().split("== 汇总 ==")[-1].strip().splitlines()[:3]
        print("    rc=%d  %s" % (rc, " | ".join(tail)), flush=True)
        open(os.path.join(d, "done"), "w").close()


if __name__ == "__main__":
    main()
