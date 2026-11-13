# Design

## Context

见 proposal.md 的 Why。现状和约束：

- **协议与采集线程**：`Sample` 已有 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate` 三个 optional 字段，M1/M2 都没有设置。`Collector` 通过 `MetricReaders` 注入内存和磁盘读取函数，失败时沿用上次的值。
- **Worker 权限**：Worker 以 `lm` 用户、通过 systemd 运行，单元已授予 `CAP_BPF CAP_PERFMON`。加载 `tp_btf` 程序需要这两个能力。libbpf 1.x 在 5.11 以上内核按 memcg 计费，不受 RLIMIT_MEMLOCK 限制。
- **目标机**（lm-worker-1）：6.8.0-149 aarch64，有 `/sys/kernel/btf/vmlinux`。已装 libbpf1 1.3.0、bpftool（`/usr/lib/linux-tools/6.8.0-149-generic/bpftool`）、bcc 0.29.1（`/usr/sbin/runqlat-bpfcc`）。
- **构建容器**：lm-builder 是 Ubuntu 24.04 aarch64，宿主内核 7.0（linuxkit），带 `/sys/kernel/btf/vmlinux`。Ubuntu 的 `/usr/sbin/bpftool` 是包装脚本，会按 `uname -r` 找对应版本的二进制，在容器里找不到。
- **开发机**：macOS 没有 eBPF，Worker 必须仍能编译和跑单元测试。

## Goals / Non-Goals

**Goals:**
- 内核程序按开发文档的骨架实现，三个 map 的类型和大小与开发文档一致。
- 分桶、差分、插值、丢弃率等纯计算放在与平台无关的代码里，在 macOS 上就能单测。
- 加载失败不影响 Worker 其他指标，行为与 M2 的 `cpu_source: auto` 一致。

**Non-Goals:**
- 不做按核或按 cgroup 的直方图，不把直方图本身上报。
- 不兼容 `task_struct.state`（5.14 之前的字段名）。

## Decisions

### 1. 目录与构建
```
bpf/runqlat.h          分桶常量和 runq_bucket_of()，BPF 与 C++ 共用（只依赖基本整数类型）
bpf/runqlat.bpf.c      内核程序
bpf/CMakeLists.txt     vmlinux.h → runqlat.bpf.o → runqlat.skel.h，导出接口库 lm_bpf_runqlat
worker/collector/runq_histogram.{h,cpp}  纯计算：桶区间、累计差分、分位数、丢弃率（各平台都编）
worker/collector/runq_probe.{h,cpp}      打开、加载、挂载 skeleton，读取并合并 map（仅在启用 eBPF 时编译实现）
worker/bench/runq_compare.cpp            对照工具
bpf/test/compare_runqlat.py              解析 runqlat-bpfcc 输出并与对照工具比较
```
- CMake 选项 `LM_ENABLE_BPF`，取值 `AUTO`（默认）、`ON`、`OFF`。AUTO 表示在 Linux 上同时找到 clang、bpftool、libbpf 时启用。启用后给 `lm_worker_collector` 加编译宏 `LM_HAVE_BPF`。
- 未启用时，`runq_probe.cpp` 编译成一个桩，`open()` 抛出“构建时未启用 eBPF”。
- **bpftool 的查找顺序**：
  1. CMake 变量 `BPFTOOL`。
  2. `/usr/lib/linux-tools/*/bpftool` 中版本最新的。
  3. PATH 里的 `bpftool`。
- **vmlinux.h**：构建时执行 `bpftool btf dump file ${LM_VMLINUX_BTF} format c` 生成，默认读 `/sys/kernel/btf/vmlinux`，可以用 `-DLM_VMLINUX_BTF=` 指向别的 BTF 文件。生成的文件放在构建目录，不进仓库。
  - CO-RE 用重定位适配目标内核的字段偏移，所以构建容器用 7.0 内核的 BTF，生成的程序在 6.8 上也能用。
  - 备选方案：把 vmlinux.h 提交进仓库。文件有数 MB，而且和架构相关，不采用。
- **编译 BPF 程序**：`clang -g -O2 -target bpf -D__TARGET_ARCH_${arm64|x86}`，再用 `bpftool gen skeleton` 生成头文件。skeleton 把目标文件内嵌进 Worker，运行时不需要额外文件。
- 构建镜像增加 `clang llvm libbpf-dev linux-tools-generic`。Worker 运行时增加 `libbpf1`。

### 2. 内核程序
- 按开发文档实现 `mark()` 和三个 `tp_btf` 程序。读 `prev->__state` 用 `BPF_CORE_READ`，读 `p->pid` 直接访问 BTF 指针。
- **`runq_bucket_of(u64 us)`**：
  - 小于 8 的值直接返回。
  - 其余的值用无循环的移位比较求 ⌊log₂⌋（和 bcc 的 `log2l` 写法一样），避免在 BPF 里依赖 `__builtin_clzll`。
  - 按规格公式算出桶号，最后截断到 207。
  - 同一份代码给用户态单测，保证两侧完全一致。
- **map 定义**：
  - `start`：`BPF_MAP_TYPE_HASH`，10240 项，key u32，value u64。
  - `hist`：`BPF_MAP_TYPE_PERCPU_ARRAY`，208 项，value u64。
  - `drops`：`BPF_MAP_TYPE_PERCPU_ARRAY`，1 项。
- **许可证**：`char LICENSE[] SEC("license") = "GPL";`。`tp_btf` 和 `bpf_ktime_get_ns` 都要求 GPL 兼容。

### 3. 用户态读取（RunqProbe）
- **打开**：`runqlat_bpf__open_and_load()`，再 `runqlat_bpf__attach()`。任一步失败就收集 libbpf 的错误码，转成 `std::runtime_error`，消息中附带 `strerror` 和失败的阶段（open/load/attach）。
- **libbpf 日志**：用 `libbpf_set_print` 转到 spdlog。warn 及以上的级别记 debug，避免 auto 模式下每次启动都刷一屏日志；失败原因在异常消息里给出。
- **读取累计值**：
  - 对 `hist` 用 `bpf_map_lookup_batch` 一次取出 208 × ncpu 个值，按 `libbpf_num_possible_cpus()` 求和。内核不支持批量查找（返回 EINVAL 或 ENOTSUP）时，退回逐个键 `bpf_map_lookup_elem`。
  - `drops` 只有一个键，直接查。
  - 结果放进 `RunqCumulative{ uint64 buckets[208]; uint64 drops; }`。
- 卸载：析构时调用 `runqlat_bpf__destroy()`。

### 4. 纯计算（runq_histogram）
- `RunqWindow::update(const RunqCumulative&) -> std::optional<RunqStats>`：
  - 第一次只存基线，返回 nullopt。
  - 之后逐桶相减。累计值不应倒退；万一倒退（例如程序被重新加载），按 0 处理，并以本次为新的基线。
- `runq_percentile(const uint64_t delta[208], double p)`：按规格的秩和线性插值计算。
  - 桶区间：b < 8 时为 [b, b+1)。否则 k = b/8 + 2，s = b%8，下界 (8+s) << (k−3)，宽度 1 << (k−3)。
- `drop_rate = Δdrops / (Δdrops + N)`，分母为 0 时取 0。

### 5. 接入 Collector
- `MetricReaders` 新增 `std::function<RunqResult()> runq`。`RunqResult` 有四种状态：
  - `kOk`（附带 stats）：设置三个字段并记住。
  - `kBaseline`：不设置。
  - `kFailed`：有上次的值就沿用并记警告（与内存、磁盘一致，连续 10 次失败升为 error），没有就不设置。
  - `kDisabled`：不设置。

  函数为空时等同 `kDisabled`。
- 读取在采集线程里完成，不新增线程。每次读取是一次批量系统调用加 208 × ncpu 次加法，规格要求不超过 1 毫秒。

### 6. Worker 配置与启动
- `runq: auto | on | off`，校验时机与 `cpu_source` 相同：在检查 /proc 之前。
- `auto`：`RunqProbe` 构造失败时记警告，`readers.runq` 留空。
- `on`：构造失败时以 critical 级别记录，退出码 1。
- 启动日志写“调度延迟采集：已启用（eBPF runqlat）”或“未启用：原因”。

### 7. Manager 写入
- `RedisWriter::write_batch` 在拼 `node:{id}` 的 HSET 字段时，对 `has_runq_*()` 为真的字段追加 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate`。
- 三个字段都没有时，在同一个 pipeline 里追加 `HDEL node:{id} runq_p50_us runq_p99_us runq_drop_rate`。
  - 三个字段由 Worker 同时设置或同时不设置，所以只需按 `has_runq_p99_us()` 判断。
- JSON 由 `MessageToJsonString` 生成。proto3 optional 字段有存在性，设置为 0 时也会输出，不需要改。
- 写入量：每次更新多 3 个字段，或者多一条 HDEL。走的是同一个 pipeline 往返，Redis 负载可以忽略。

### 8. 对照工具与比较脚本
- `runq_compare --seconds 30`：
  1. 加载 RunqProbe，读一次基线，睡指定秒数，再读一次。
  2. 输出 JSON：208 桶差分、N、drops、P50、P99。
- `bpf/test/compare_runqlat.py runqlat.txt ours.json`：
  1. 解析 `runqlat-bpfcc` 打印的 `low -> high : count` 行。
  2. 把对照工具的每个桶按下界归入 runqlat 的同一区间。
  3. 两边各自归一化，输出总变差距离，以及按 2 的幂区间插值得到的 P50、P99。
  4. 按规格阈值判定 PASS/FAIL。
- **时间对齐**：先启动 `runqlat-bpfcc 30 1`，等它打印 “Tracing” 后再启动对照工具（bcc 启动时要编译，约 1–3 秒）。两边窗口错开不到 1 秒，相对于 30 秒窗口可以忽略。

### 9. 实现中补充的决定（2026-11-13）
- **BPF 编译加 `-Wno-missing-declarations`**：用 7.0 内核 BTF 生成的 vmlinux.h 里有匿名声明，在 `-Werror` 下会报错。只关这一类警告，其余仍然 `-Werror`。
- **日志改写到 stderr**：`worker --dry-run` 在标准输出上打印样本 JSON，日志混在里面会让下游解析失败。systemd 下 stdout 和 stderr 都进 journal，不受影响。这一改动作用于所有组件（`common/src/log.cpp`）。
- **首条样本就有调度延迟**：第一次采集同时建立 CPU 与调度延迟的基线，不出样本，所以输出的第一条样本已经带三个字段。规格措辞据此修正。
- **旧配置不含 `runq`**：`install-worker.sh` 默认保留目标机已有的 `/etc/lm/worker.yaml`，老配置里没有 `runq` 行，按默认值 `auto` 处理，行为符合预期。
- **Manager 镜像**：Dockerfile 补拷 `kmod/cpustat/cpustat_uapi.h` 和 `bpf/`，并显式加 `-DLM_ENABLE_BPF=OFF`。Manager 不用 eBPF，构建阶段也没有 clang。

## 性能与资源影响

- **内核开销**：每次唤醒和切换各执行一次 BPF 程序，包括一次 HASH 更新、查找或删除，和一次 per-CPU 计数。这与 bcc runqlat 相同，属于业界常规开销。
  - 规格只约束 Worker 自身的 CPU 占用。用户态读取每秒一次，占用很小。
  - 调度密集场景的额外开销用 `perf bench sched pipe` 测量并记录，作为参考数据，不设门槛。
- **内存**：`start` 约 10240 × (4+8) 字节加哈希表开销，`hist` 为 208 × 8 × ncpu 字节，总共不到 1 MB。
- 上报频率、Manager 锁、MySQL 都不变。Redis 每次更新多 3 个字段。

## Risks / Trade-offs

- [`start` 表里残留不会再被换上 CPU 的任务的条目] → 任务要运行才能退出，退出前一定会被换上 CPU，条目随之删除，所以残留只会在极端情况下出现。丢弃计数会把表满暴露出来，异常检测会把丢弃率超过 1% 当作事件（M8）。
- [构建容器的 BTF 来自 7.0 内核] → CO-RE 重定位处理偏移差异。用到的 `task_struct.pid` 和 `__state` 在 6.8 和 7.0 中都存在。虚拟机验收时确认加载成功。
- [Ubuntu 的 bpftool 包装脚本在容器里不可用] → CMake 直接查找 `/usr/lib/linux-tools/*/bpftool`。
- [与 bcc runqlat 的口径细节不同] → bcc 判断 `prev->state == TASK_RUNNING`，不看 preempt 标志；本实现按开发文档两者都看。差异只在少数边界事件上，用总变差距离 ≤ 0.1 作为接近的标准。
- [macOS 构建不含 eBPF] → 桩实现加上 auto 降级，纯计算部分仍然在 macOS 上单测。

## Migration Plan

1. 先升级 Manager（多写 3 个字段，对老 Worker 无影响），再用 `install-worker.sh` 升级 Worker（会装 libbpf1）。
2. 回滚：在 Worker 配置里写 `runq: off` 并重启，或者换回 M2 的二进制。Manager 会在下一条样本时删掉 Hash 中的三个字段。
