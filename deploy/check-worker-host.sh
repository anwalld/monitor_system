#!/usr/bin/env bash
# Worker 主机环境自检：只读检查，不修改系统。
# 用法：deploy/check-worker-host.sh
# 退出码：全部满足为 0，任一项不满足为 1。

set -u

MIN_KERNEL_MAJOR=6
MIN_KERNEL_MINOR=8
MIN_NICS=2

failed=0

report() {
  # report <检查项> <OK|FAIL> <实际值>
  printf '%-10s %-4s %s\n' "$1" "$2" "$3"
  if [[ "$2" != "OK" ]]; then
    failed=1
  fi
}

check_kernel() {
  local release major minor
  release="$(uname -r)"
  major="${release%%.*}"
  minor="${release#*.}"
  minor="${minor%%.*}"
  minor="${minor%%[!0-9]*}"
  if [[ "$major" =~ ^[0-9]+$ && "$minor" =~ ^[0-9]+$ ]] &&
     (( major > MIN_KERNEL_MAJOR || (major == MIN_KERNEL_MAJOR && minor >= MIN_KERNEL_MINOR) )); then
    report kernel OK "$release"
  else
    report kernel FAIL "$release（需要 >= ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR}）"
  fi
}

check_btf() {
  if [[ -r /sys/kernel/btf/vmlinux ]]; then
    report btf OK "/sys/kernel/btf/vmlinux"
  else
    report btf FAIL "/sys/kernel/btf/vmlinux 不存在或不可读"
  fi
}

kernel_config_value() {
  # 输出某个内核配置项的值（y/m/空）
  local key="$1" cfg
  cfg="/boot/config-$(uname -r)"
  if [[ -r "$cfg" ]]; then
    grep -E "^${key}=" "$cfg" | head -n1 | cut -d= -f2
  elif [[ -r /proc/config.gz ]]; then
    zcat /proc/config.gz | grep -E "^${key}=" | head -n1 | cut -d= -f2
  else
    echo "unknown"
  fi
}

check_nohz() {
  local v
  v="$(kernel_config_value CONFIG_NO_HZ_COMMON)"
  case "$v" in
    y) report nohz OK "CONFIG_NO_HZ_COMMON=y" ;;
    unknown) report nohz FAIL "找不到内核配置（/boot/config-$(uname -r) 与 /proc/config.gz 都不可读）" ;;
    *) report nohz FAIL "CONFIG_NO_HZ_COMMON=${v:-未设置}" ;;
  esac
}

check_bpftool() {
  if ! command -v bpftool >/dev/null 2>&1; then
    report bpftool FAIL "未安装 bpftool"
    return
  fi
  if [[ ! -r /sys/kernel/btf/vmlinux ]]; then
    report bpftool FAIL "缺少 BTF，无法 dump"
    return
  fi
  local lines
  lines="$(bpftool btf dump file /sys/kernel/btf/vmlinux format c 2>/dev/null | head -n 20 | wc -l)"
  if (( lines > 0 )); then
    report bpftool OK "$(bpftool version 2>/dev/null | head -n1)"
  else
    report bpftool FAIL "bpftool btf dump 无输出（可能需要 root 或 CAP_BPF）"
  fi
}

check_nics() {
  # 除回环外、处于 UP 状态且有 IPv4 地址的网卡
  local nics=() name
  while read -r name; do
    [[ -n "$name" ]] && nics+=("$name")
  done < <(ip -o -4 addr show up 2>/dev/null | awk '$2 != "lo" {print $2}' | sort -u)
  local list="${nics[*]:-}"
  if (( ${#nics[@]} >= MIN_NICS )); then
    report nics OK "${#nics[@]} 块：${list}"
  else
    report nics FAIL "${#nics[@]} 块：${list:-无}（需要 >= ${MIN_NICS}）"
  fi
}

check_kernel
check_btf
check_nohz
check_bpftool
check_nics

exit "$failed"
