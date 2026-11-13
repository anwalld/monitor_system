# Proposal

## Why

对应开发文档里程碑 M2。M1 的 Worker 每秒 `pread` 并解析 `/proc/stat` 取 CPU 读数，每次要格式化、解析整段文本。M6 急救时要把采样周期压到 1 毫秒，这个开销就扛不住了。开发文档的方案是：内核模块定时把每核空闲时间写进一块只读共享页，Worker 用 seqlock 协议直接读内存，读取耗时要比 `/proc/stat` 低 85% 以上。M1 已经把 CPU 读取抽象成 `CpuSource` 接口，现在可以只换来源、不动上层。

## What Changes

- 新增 `kmod/cpustat/`：内核模块 `cpustat.c`、内核与用户态共用的 `cpustat_uapi.h`、Kbuild `Makefile`。
  - 模块注册只读字符设备 `/dev/cpustat`，用 `vmalloc_user` 分配共享页供 mmap。
  - 软中断 hrtimer 按周期刷新每核 `idle_us`、`iowait_us`、`stamp_us`，用 seq 奇偶协议保护。
  - ioctl `CPUSTAT_SET_INTERVAL` 修改刷新周期。
  - 内核没开 NO_HZ 时拒绝加载。
- 新增 `common/` 中的 seqlock 读端：一致性读取共享页快照，最多重试 100 次。
- Worker 新增 `ShmCpuSource`，实现 `CpuSource`，以 `stamp_us` 作为真实时间差。
  - 新增配置项 `cpu_source: auto | shm | proc`，默认 `auto`：有 `/dev/cpustat` 就读共享页，没有就回退到 `/proc/stat` 并记一条警告。
  - 样本格式和忙碌度公式不变。
- 新增刷新周期守卫：作用域内把周期改为 1 毫秒，离开作用域时改回 1 秒。M2 只提供这个守卫并测试，M6 急救时才使用。
- 新增读取耗时基准程序：对比共享页和 `/proc/stat` 两种来源，输出 P50 和 P99。
- 部署：
  - 新增 udev 规则，让 `lm` 组可读 `/dev/cpustat`。
  - `install-worker.sh` 增加编译、安装、开机加载模块的步骤。
  - `lm-worker.service` 排在模块加载之后启动。

## Capabilities

### New Capabilities
- `kmod-cpustat`：内核模块对外的契约，包括设备节点与权限、共享页布局、seq 协议、刷新周期与 ioctl、加载卸载约束。
- `worker-cpu-source`：Worker 选择 CPU 读数来源、按 seqlock 一致性读取共享页、回退到 `/proc` 的行为，以及刷新周期守卫。

### Modified Capabilities
（无。M1 的 `worker-metric-collection` 只规定忙碌度公式和采集周期，不规定来源，M2 不改这些要求。M1 尚未归档，`openspec/specs` 目前为空。）

## Impact

- 对应里程碑：M2。影响模块：kmod（新增）、common、worker、deploy。
- 不涉及 proto、Redis key、MySQL 表结构，Manager 不改。上报协议和样本字段完全兼容。
- 新的构建依赖：Worker 主机需要 `linux-headers-$(uname -r)`、`build-essential`。内核模块不进 CMake，用 Kbuild 单独编译，所以 macOS 上的 CMake 构建不受影响。
- 不在范围内：
  - 急救流程本身，以及什么时候切到 1 毫秒周期（M6）。
  - 调度延迟和 eBPF（M3）。
  - DKMS 打包和内核升级后自动重编（后续视需要再加）。
  - Secure Boot 环境下的模块签名：目标虚拟机关闭了 Secure Boot，在部署文档里说明即可。
