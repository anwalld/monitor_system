#!/usr/bin/env bash
# Worker 多网卡策略路由：每块网卡一张路由表，按源地址和出接口分流。
#
# 用法：
#   sudo deploy/policy-route.sh --nic eth0 --nic eth1:192.168.2.1   # 建立
#   sudo deploy/policy-route.sh                                     # 从配置文件读取网卡
#   sudo deploy/policy-route.sh --down                              # 撤销
#
# 配置文件（默认 deploy/policy-route.conf，可用 --conf 指定）每行：<网卡> [网关]，# 开头为注释。
# 第 i 块网卡（从 0 起）：路由表 100+i，from 规则优先级 1000+i，oif 规则优先级 1100+i。
# 可重复执行；--down 删除本脚本添加的规则与路由表内容，并恢复 rp_filter 原值。

set -euo pipefail

TABLE_BASE=100
FROM_PRIO_BASE=1000
OIF_PRIO_BASE=1100
STATE_FILE=/run/lm-policy-route.state
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="${SCRIPT_DIR}/policy-route.conf"

MODE=up
NIC_SPECS=()

die() { echo "错误：$*" >&2; exit 1; }
log() { echo "$*"; }

usage() {
  sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

parse_args() {
  while (( $# > 0 )); do
    case "$1" in
      --nic) [[ $# -ge 2 ]] || die "--nic 需要参数"; NIC_SPECS+=("$2"); shift 2 ;;
      --conf) [[ $# -ge 2 ]] || die "--conf 需要参数"; CONF_FILE="$2"; shift 2 ;;
      --down) MODE=down; shift ;;
      -h|--help) usage 0 ;;
      *) echo "未知参数：$1" >&2; usage 1 ;;
    esac
  done
}

load_conf() {
  [[ -r "$CONF_FILE" ]] || return 0
  local nic gw
  while read -r nic gw _; do
    [[ -z "${nic:-}" || "$nic" == \#* ]] && continue
    if [[ -n "${gw:-}" ]]; then NIC_SPECS+=("${nic}:${gw}"); else NIC_SPECS+=("$nic"); fi
  done < "$CONF_FILE"
}

# 删除某优先级的全部规则（可能因手工操作残留多条）
del_rules_at() {
  local prio="$1"
  while ip -4 rule del priority "$prio" 2>/dev/null; do :; done
}

nic_ipv4() {
  ip -4 -o addr show dev "$1" 2>/dev/null | awk '{print $4}' | head -n1
}

infer_gateway() {
  ip -4 route show default dev "$1" 2>/dev/null | awk '/via/ {for (i=1;i<=NF;i++) if ($i=="via") {print $(i+1); exit}}'
}

# 已连接网段（内核为地址自动生成的 scope link 路由）
nic_subnet() {
  ip -4 route show dev "$1" scope link proto kernel 2>/dev/null | awk '{print $1}' | head -n1
}

do_up() {
  (( ${#NIC_SPECS[@]} > 0 )) || die "没有指定网卡：用 --nic 或在 ${CONF_FILE} 中配置"

  # 第一步：解析并校验全部网卡，任何一块失败都不改动系统
  local names=() cidrs=() gws=() subnets=()
  local spec nic gw cidr subnet
  for spec in "${NIC_SPECS[@]}"; do
    nic="${spec%%:*}"
    gw=""
    [[ "$spec" == *:* ]] && gw="${spec#*:}"
    ip link show dev "$nic" >/dev/null 2>&1 || die "网卡 ${nic} 不存在"
    cidr="$(nic_ipv4 "$nic")"
    [[ -n "$cidr" ]] || die "网卡 ${nic} 没有 IPv4 地址"
    [[ -n "$gw" ]] || gw="$(infer_gateway "$nic")"
    [[ -n "$gw" ]] || die "网卡 ${nic} 未指定网关，也无法从现有路由推断；请用 --nic ${nic}:<网关>"
    subnet="$(nic_subnet "$nic")"
    names+=("$nic"); cidrs+=("$cidr"); gws+=("$gw"); subnets+=("$subnet")
  done

  # 第二步：记录 rp_filter 原值（已有状态文件时保留最初记录的值，保证多次执行后仍能恢复）
  local i
  if [[ ! -f "$STATE_FILE" ]]; then
    : > "$STATE_FILE"
    for nic in "${names[@]}"; do
      echo "$nic $(sysctl -n "net.ipv4.conf.${nic}.rp_filter")" >> "$STATE_FILE"
    done
  else
    for nic in "${names[@]}"; do
      grep -q "^${nic} " "$STATE_FILE" ||
        echo "$nic $(sysctl -n "net.ipv4.conf.${nic}.rp_filter")" >> "$STATE_FILE"
    done
  fi

  # 第三步：先清理再重建，保证幂等
  for i in "${!names[@]}"; do
    nic="${names[$i]}"
    local table=$((TABLE_BASE + i)) ip="${cidrs[$i]%/*}"
    del_rules_at $((FROM_PRIO_BASE + i))
    del_rules_at $((OIF_PRIO_BASE + i))
    ip -4 route flush table "$table" 2>/dev/null || true

    if [[ -n "${subnets[$i]}" ]]; then
      ip -4 route add "${subnets[$i]}" dev "$nic" src "$ip" table "$table"
    fi
    ip -4 route add default via "${gws[$i]}" dev "$nic" src "$ip" table "$table"
    ip -4 rule add from "$ip" table "$table" priority $((FROM_PRIO_BASE + i))
    ip -4 rule add oif "$nic" table "$table" priority $((OIF_PRIO_BASE + i))
    sysctl -qw "net.ipv4.conf.${nic}.rp_filter=2"

    log "${nic}: ip=${ip} gw=${gws[$i]} table=${table} prio=from:$((FROM_PRIO_BASE + i)) oif:$((OIF_PRIO_BASE + i))"
  done
  ip -4 route flush cache 2>/dev/null || true
}

do_down() {
  # 按固定编号清理：网卡数量上限取 16，覆盖配置变化后残留的规则
  local i
  for (( i = 0; i < 16; i++ )); do
    del_rules_at $((FROM_PRIO_BASE + i))
    del_rules_at $((OIF_PRIO_BASE + i))
    ip -4 route flush table $((TABLE_BASE + i)) 2>/dev/null || true
  done

  if [[ -f "$STATE_FILE" ]]; then
    local nic val
    while read -r nic val; do
      [[ -n "$nic" && -n "$val" ]] || continue
      if [[ -e "/proc/sys/net/ipv4/conf/${nic}/rp_filter" ]]; then
        sysctl -qw "net.ipv4.conf.${nic}.rp_filter=${val}"
        log "${nic}: rp_filter 恢复为 ${val}"
      fi
    done < "$STATE_FILE"
    rm -f "$STATE_FILE"
  fi
  ip -4 route flush cache 2>/dev/null || true
  log "策略路由已撤销"
}

main() {
  parse_args "$@"
  [[ "$(uname -s)" == "Linux" ]] || die "只能在 Linux 上运行"
  (( EUID == 0 )) || die "需要 root 权限"
  if [[ "$MODE" == down ]]; then
    do_down
  else
    (( ${#NIC_SPECS[@]} > 0 )) || load_conf
    do_up
  fi
}

main "$@"
