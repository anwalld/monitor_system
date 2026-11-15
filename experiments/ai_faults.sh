#!/bin/bash
# M8 异常检测验收：在 Worker 虚拟机上依次注入故障，把每次注入的起止时刻写成 JSONL，交给 ai_eval.py 统计。
# 用法：experiments/ai_faults.sh [-h ubuntu@172.16.255.133] [-r 轮数] [-d 每次秒数] [-g 间隔秒数] [-o out.jsonl] [故障...]
#      experiments/ai_faults.sh -c lmtest-worker2 -n lmtest-worker2 -i eth1 [...]   # 对 Mac 上的测试 Worker 容器注入
# 故障：cpu runq mem disk nic stop（默认全部）。
# 虚拟机：nic 用 ens192 netem（先设 systemd-run 自动撤销），stop 停 lm-worker 服务。
# 容器：命令用 docker exec 执行（容器里的 /proc 是 Docker 虚拟机全局的，负载能被 Worker 看到）；mem 只吃 30% 内存，
#      避免 OOM 波及同一 Docker 虚拟机里的其他容器；nic 在容器里对 -i 指定的网卡做 netem（需要 NET_ADMIN）；stop 用 docker pause。
set -euo pipefail

HOST=ubuntu@172.16.255.133
ROUNDS=2
DUR=60
GAP=120
OUT=ai_faults.jsonl
NODE=lm-worker-1
NIC=ens192
CONTAINER=""
while getopts "h:r:d:g:o:n:c:i:" o; do
  case $o in
    h) HOST=$OPTARG ;; r) ROUNDS=$OPTARG ;; d) DUR=$OPTARG ;; g) GAP=$OPTARG ;; o) OUT=$OPTARG ;; n) NODE=$OPTARG ;;
    c) CONTAINER=$OPTARG ;; i) NIC=$OPTARG ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
FAULTS=("$@")
[ ${#FAULTS[@]} -eq 0 ] && FAULTS=(cpu runq mem disk nic stop)

now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }
remote() { ssh -o ConnectTimeout=10 "$HOST" "$@"; }
inc() { docker exec "$CONTAINER" bash -c "$1"; }

inject_container() {
  case $1 in
    cpu)  inc "stress -c \$(nproc) --timeout ${DUR}s >/dev/null 2>&1" ;;
    runq) inc "taskset -c 0 stress -c 8 --timeout ${DUR}s >/dev/null 2>&1" ;;
    mem)  inc "m=\$(awk '/MemTotal/{print int(\$2/1024*0.30)}' /proc/meminfo); stress --vm 1 --vm-bytes \${m}M --vm-keep --timeout ${DUR}s >/dev/null 2>&1" ;;
    disk) inc "fio --name=aieval --filename=/tmp/aieval.fio --size=1G --rw=randwrite --bs=4k --direct=1 --ioengine=libaio --iodepth=16 --runtime=${DUR} --time_based >/dev/null 2>&1; rm -f /tmp/aieval.fio" ;;
    nic)  inc "tc qdisc replace dev $NIC root netem delay 100ms loss 30% && sleep $DUR; tc qdisc del dev $NIC root" ;;
    stop) docker pause "$CONTAINER" >/dev/null; sleep "$DUR"; docker unpause "$CONTAINER" >/dev/null ;;
    *) echo "未知故障 $1" >&2; return 1 ;;
  esac
}

inject() {  # 阻塞到故障结束
  if [ -n "$CONTAINER" ]; then inject_container "$1"; return; fi
  case $1 in
    cpu)  remote "stress -c 4 --timeout ${DUR}s >/dev/null 2>&1" ;;
    runq) remote "taskset -c 0 stress -c 8 --timeout ${DUR}s >/dev/null 2>&1" ;;
    mem)  # 吃掉约 85% 的可用内存
          remote "avail=\$(awk '/MemAvailable/{print int(\$2/1024*0.85)}' /proc/meminfo); stress --vm 1 --vm-bytes \${avail}M --vm-keep --timeout ${DUR}s >/dev/null 2>&1" ;;
    disk) remote "fio --name=aieval --filename=/tmp/aieval.fio --size=1G --rw=randwrite --bs=4k --direct=1 --ioengine=libaio --iodepth=16 --runtime=${DUR} --time_based >/dev/null 2>&1; rm -f /tmp/aieval.fio" ;;
    nic)  remote "sudo systemd-run --quiet --on-active=$((DUR + 60)) /usr/sbin/tc qdisc del dev $NIC root 2>/dev/null; \
                  sudo tc qdisc replace dev $NIC root netem delay 100ms loss 30% && sleep $DUR; sudo tc qdisc del dev $NIC root" ;;
    stop) remote "sudo systemctl stop lm-worker; sleep $DUR; sudo systemctl start lm-worker" ;;
    *) echo "未知故障 $1" >&2; return 1 ;;
  esac
}

for r in $(seq 1 "$ROUNDS"); do
  for f in "${FAULTS[@]}"; do
    s=$(now_ms)
    echo "$(date '+%T') 第 $r 轮 $f 开始（${DUR}s）" >&2
    inject "$f"
    e=$(now_ms)
    printf '{"fault":"%s","node":"%s","round":%d,"start_ms":%s,"end_ms":%s}\n' "$f" "$NODE" "$r" "$s" "$e" >> "$OUT"
    echo "$(date '+%T') $f 结束，等待 ${GAP}s" >&2
    sleep "$GAP"
  done
done
echo "完成，记录在 $OUT" >&2
