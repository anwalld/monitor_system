#!/usr/bin/env bash
# cpustat 卸载安全与泄漏检查（需要 root，模块须未加载）。
#   sudo kmod/cpustat/test/load_loop.sh [次数，默认 1000]
# 1. 映射期间 rmmod 应失败；
# 2. 循环 insmod/rmmod，每次卸载后 /proc/vmallocinfo 中不应有 cpustat 的分配；
# 3. 比较循环前后的 VmallocUsed 与 Slab（增长不超过 64 KB 视为通过，Slab 只作参考）；
# 4. 内核开启 kmemleak 时追加一次扫描。
set -euo pipefail

N=${1:-1000}
DIR=$(cd "$(dirname "$0")/.." && pwd)
KO=$DIR/cpustat.ko
TEST=$DIR/test/cpustat_test
fail=0

[[ $EUID -eq 0 ]] || { echo "需要 root"; exit 2; }
[[ -f $KO && -x $TEST ]] || { echo "先在 $DIR 下执行 make && make test"; exit 2; }
if grep -q '^cpustat ' /proc/modules; then echo "请先 rmmod cpustat"; exit 2; fi

meminfo() { awk -v k="$1:" '$1==k{print $2}' /proc/meminfo; }

# 1. 映射期间卸载
insmod "$KO"
"$TEST" --hold 3 >/dev/null &
holder=$!
sleep 1
if rmmod cpustat 2>/dev/null; then
  echo "FAIL  rmmod succeeded while mapped"; fail=1
else
  echo "PASS  rmmod refused while mapped"
fi
wait "$holder"
rmmod cpustat
echo "PASS  rmmod succeeded after mapping released"

# 2. 循环加载卸载
sync; echo 3 >/proc/sys/vm/drop_caches
v0=$(meminfo VmallocUsed); s0=$(meminfo Slab)
for ((i = 1; i <= N; i++)); do
  insmod "$KO"
  rmmod cpustat
  if grep -q 'cpustat' /proc/vmallocinfo; then
    echo "FAIL  iteration $i: cpustat allocation left in /proc/vmallocinfo"; fail=1; break
  fi
done
sync; echo 3 >/proc/sys/vm/drop_caches
v1=$(meminfo VmallocUsed); s1=$(meminfo Slab)
dv=$((v1 - v0)); ds=$((s1 - s0))
echo "INFO  $N cycles: VmallocUsed ${v0} -> ${v1} kB (${dv}), Slab ${s0} -> ${s1} kB (${ds})"
if ((dv > 64)); then echo "FAIL  VmallocUsed grew ${dv} kB"; fail=1; else echo "PASS  VmallocUsed growth ${dv} kB <= 64"; fi

# 3. kmemleak（可选）
if [[ -w /sys/kernel/debug/kmemleak ]]; then
  echo scan >/sys/kernel/debug/kmemleak
  if grep -q cpustat /sys/kernel/debug/kmemleak; then
    echo "FAIL  kmemleak reports cpustat"; fail=1
  else
    echo "PASS  kmemleak: no cpustat leaks"
  fi
else
  echo "SKIP  kmemleak not available on this kernel (CONFIG_DEBUG_KMEMLEAK off)"
fi

((fail == 0)) && echo "ALL PASS" || echo "FAILED"
exit $fail
