# Spec Delta

## Purpose

在部署 Worker 之前判定一台主机是否具备运行 cpustat 内核模块、CO-RE eBPF 程序和多网卡链路探测的条件，避免到加载阶段才失败。

## ADDED Requirements

### Requirement: 逐项检查 Worker 主机前置条件
自检 SHALL 依次检查：内核版本不低于 6.8；`/sys/kernel/btf/vmlinux` 存在；内核配置开启 NO_HZ（`CONFIG_NO_HZ_COMMON=y`）；`bpftool btf dump file /sys/kernel/btf/vmlinux format c` 可执行；除回环外处于 UP 状态且有 IPv4 地址的网卡不少于 2 块。每一项 MUST 输出一行，包含检查项名称、结果（OK/FAIL）和实际值。

#### Scenario: 全部满足
- **WHEN** 在 Ubuntu 24.04（内核 6.8、开启 BTF 与 NO_HZ、2 块已配置地址的网卡）上运行自检
- **THEN** 每项都输出 OK，退出码为 0

#### Scenario: 缺少 BTF
- **WHEN** 主机没有 `/sys/kernel/btf/vmlinux`
- **THEN** BTF 一项输出 FAIL 并说明文件不存在，其余项照常检查，最终退出码非 0

#### Scenario: 只有一块网卡
- **WHEN** 主机除回环外只有 1 块带 IPv4 地址的 UP 网卡
- **THEN** 网卡一项输出 FAIL 并给出实际数量 1，退出码非 0

### Requirement: 自检不修改系统
自检 MUST 只读取系统状态，不加载模块、不修改路由、不安装软件包，可在任意时刻重复执行。

#### Scenario: 重复执行
- **WHEN** 连续运行两次自检
- **THEN** 两次输出一致，且运行前后 `ip rule`、`ip route show table all`、`lsmod` 的输出不变
