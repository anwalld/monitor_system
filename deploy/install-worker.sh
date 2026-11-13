#!/usr/bin/env bash
# 经 ssh 把 worker 批量部署到多台 Worker 机，并用 systemd 拉起。
#
# 用法：
#   deploy/install-worker.sh -m <manager_ip:50051> [-b <worker 二进制>] [-u <ssh 用户>] [--force-config] host1 [host2 ...]
#
#   -m  Manager gRPC 地址，写入新生成的 /etc/lm/worker.yaml（必填）
#   -b  worker 可执行文件，默认 build-linux/worker/worker；必须与目标机 CPU 架构一致
#   -u  ssh 用户，默认当前用户；需要免密 sudo
#   --force-config  覆盖目标机上已有的 /etc/lm/worker.yaml（默认保留）
#
# 目标机上：安装运行时库，创建系统用户 lm，二进制装到 /opt/lm/bin/worker，
# 部署脚本装到 /opt/lm/deploy，配置在 /etc/lm/worker.yaml（node_id 留空 = 主机名），
# 启用并重启 lm-worker.service。

# shellcheck disable=SC2029  # ssh 命令里的变量都是有意在本地展开的
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

MANAGER=""
BINARY="${REPO_DIR}/build-linux/worker/worker"
SSH_USER="${USER:-}"
FORCE_CONFIG=0
HOSTS=()

RUNTIME_PKGS="libgrpc++1.51t64 libgrpc29t64 libprotobuf32t64 libabsl20220623t64 libre2-10 libcares2 libspdlog1.12 libfmt9 libyaml-cpp0.8 libssl3t64 zlib1g"

die() { echo "错误：$*" >&2; exit 1; }

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while (( $# > 0 )); do
  case "$1" in
    -m) [[ $# -ge 2 ]] || die "-m 需要参数"; MANAGER="$2"; shift 2 ;;
    -b) [[ $# -ge 2 ]] || die "-b 需要参数"; BINARY="$2"; shift 2 ;;
    -u) [[ $# -ge 2 ]] || die "-u 需要参数"; SSH_USER="$2"; shift 2 ;;
    --force-config) FORCE_CONFIG=1; shift ;;
    -h|--help) usage 0 ;;
    -*) echo "未知参数：$1" >&2; usage 1 ;;
    *) HOSTS+=("$1"); shift ;;
  esac
done

[[ -n "$MANAGER" ]] || die "缺少 -m <manager 地址>"
(( ${#HOSTS[@]} > 0 )) || die "至少指定一台目标机"
[[ -x "$BINARY" ]] || die "找不到可执行文件 ${BINARY}（先在与目标机同架构的 Linux 上构建）"

# 二进制的 CPU 架构（读 ELF 头 e_machine）
binary_arch() {
  local m
  m="$(od -An -t x2 -j 18 -N 2 "$1" | tr -d ' ')"
  case "$m" in
    003e) echo x86_64 ;;
    00b7) echo aarch64 ;;
    *) echo "unknown($m)" ;;
  esac
}
BIN_ARCH="$(binary_arch "$BINARY")"

WORKER_YAML="$(cat <<EOF
# 由 install-worker.sh 生成；可按需修改后 systemctl restart lm-worker
node_id: ""                    # 留空则用主机名
manager_addr: ${MANAGER}
collect_interval_ms: 1000
report_interval_ms: 1000
report_deadline_ms: 500
log_level: info
EOF
)"

install_one() {
  local host="$1" target="${SSH_USER:+${SSH_USER}@}$1" stage="/tmp/lm-install.$$"
  echo "==> ${host}"

  local remote_arch
  remote_arch="$(ssh "$target" uname -m)"
  [[ "$remote_arch" == "$BIN_ARCH" ]] ||
    { echo "    跳过：目标机架构 ${remote_arch} 与二进制 ${BIN_ARCH} 不一致" >&2; return 1; }

  ssh "$target" "rm -rf ${stage} && mkdir -p ${stage}/deploy/systemd"
  scp -q "$BINARY" "${target}:${stage}/worker"
  scp -q "${SCRIPT_DIR}/check-worker-host.sh" "${SCRIPT_DIR}/policy-route.sh" \
      "${SCRIPT_DIR}/policy-route.conf.example" "${target}:${stage}/deploy/"
  scp -q "${SCRIPT_DIR}/systemd/lm-worker.service" "${SCRIPT_DIR}/systemd/lm-policy-route.service" \
      "${target}:${stage}/deploy/systemd/"
  printf '%s\n' "$WORKER_YAML" | ssh "$target" "cat > ${stage}/worker.yaml"

  # shellcheck disable=SC2087  # 变量有意在本地展开
  ssh "$target" sudo bash -s <<EOF
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq ${RUNTIME_PKGS} >/dev/null
id lm >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin lm
install -d -m 755 /opt/lm/bin /opt/lm/deploy/systemd /etc/lm
install -m 755 ${stage}/worker /opt/lm/bin/worker
install -m 755 ${stage}/deploy/check-worker-host.sh ${stage}/deploy/policy-route.sh /opt/lm/deploy/
install -m 644 ${stage}/deploy/policy-route.conf.example /opt/lm/deploy/
install -m 644 ${stage}/deploy/systemd/*.service /opt/lm/deploy/systemd/
install -m 644 ${stage}/deploy/systemd/lm-worker.service /etc/systemd/system/lm-worker.service
if [[ ! -f /etc/lm/worker.yaml || ${FORCE_CONFIG} -eq 1 ]]; then
  install -m 644 ${stage}/worker.yaml /etc/lm/worker.yaml
fi
rm -rf ${stage}
systemctl daemon-reload
systemctl enable lm-worker >/dev/null 2>&1
systemctl restart lm-worker
sleep 2
systemctl is-active --quiet lm-worker && echo "    lm-worker 运行中" || { journalctl -u lm-worker -n 20 --no-pager; exit 1; }
EOF
}

failed=()
for h in "${HOSTS[@]}"; do
  install_one "$h" || failed+=("$h")
done

if (( ${#failed[@]} > 0 )); then
  echo "以下主机部署失败：${failed[*]}" >&2
  exit 1
fi
echo "全部 ${#HOSTS[@]} 台部署完成"
