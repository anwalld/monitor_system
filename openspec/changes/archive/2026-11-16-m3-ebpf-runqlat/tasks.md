# Tasks

## 1. 构建环境与分桶公共头（bpf）

- [x] 1.1 构建镜像增加 `clang llvm libbpf-dev linux-tools-generic`。验证：重建 `lm-builder` 后容器内 `clang --version`、`pkg-config --modversion libbpf` 有输出，`/usr/lib/linux-tools/*/bpftool version` 可执行
  > 2026-11-13 完成：clang 18.1.3、libbpf 1.3.0、bpftool 在 `/usr/lib/linux-tools/6.8.0-149-generic/`。
- [x] 1.2 新增 `bpf/runqlat.h`：常量（208 桶、10240 项）、无循环的 `runq_bucket_of()`，能同时被 BPF C 和 C++ 包含。gtest 断言规格里的桶边界（0、7、8、15、16、36、63、207），并对 0–2²⁰ 全量比对一份参考实现。验证：macOS 上 ctest 通过
  > 完成：`RunqBucket.*` 覆盖规格边界，并对 0–2²⁰ 与参考实现全量比对，同时检查桶区间首尾相接。
- [x] 1.3 新增 `bpf/CMakeLists.txt` 和顶层选项 `LM_ENABLE_BPF`（AUTO/ON/OFF）：查找 clang、bpftool（按 design 的顺序）、libbpf；生成 vmlinux.h → runqlat.bpf.o → runqlat.skel.h；导出 `lm_bpf_runqlat`。AUTO 时缺任何一项都打印原因并跳过。验证：macOS 上 configure 显示“eBPF：未启用（非 Linux）”且构建通过；容器内 configure 显示已启用
  > 完成：macOS 上显示“eBPF：未启用（非 Linux）”；容器里显示已启用。vmlinux.h 的匿名声明需要加 `-Wno-missing-declarations`（见 design 第 9 节）。

## 2. 内核程序（bpf）

- [x] 2.1 编写 `bpf/runqlat.bpf.c`：三个 map、`mark()`、`on_wakeup`、`on_wakeup_new`、`on_switch`，许可证 GPL。验证：容器内能生成 `runqlat.skel.h`；【Linux】在 lm-worker-1 上用 `sudo bpftool prog load` 或对照工具加载，确认通过验证器，`bpftool prog show` 能看到三个程序
  > 完成：在 lm-worker-1 上以 root 运行 `runq_compare`，`bpftool prog show` 列出 on_wakeup、on_wakeup_new、on_switch 三个 tracing 程序，验证器通过。

## 3. Worker 采集（worker）

- [x] 3.1 实现 `worker/collector/runq_histogram.{h,cpp}`：`RunqCumulative`、`RunqWindow::update`（基线、逐桶差分、倒退时重建基线）、`runq_percentile`（秩加桶内线性插值）、丢弃率。验证：gtest 覆盖规格的插值例子（第 16 桶 100 个样本，P50=17.0、P99=17.98）、周期差分、空闲周期全 0、丢弃率 0.01、计数倒退
  > 完成：`RunqPercentile.*`、`RunqWindow.*`、`RunqDropRate.*`。
- [x] 3.2 实现 `worker/collector/runq_probe.{h,cpp}`：打开、加载、挂载，失败时异常消息注明阶段和原因；libbpf 日志转 spdlog；批量读取 `hist` 并在失败时退回逐个键读取，读取 `drops`；未启用 eBPF 时编译为桩。验证：macOS 上桩的 `open()` 抛出“构建时未启用 eBPF”；【Linux】见 5.2
  > 完成：macOS 上桩报“构建时未启用 eBPF”；虚拟机上批量读取可用（`batch: true`）。
- [x] 3.3 `Collector` 接入 `RunqResult`（kOk、kBaseline、kFailed、kDisabled），失败时沿用上次的值，连续 10 次升为 error。验证：gtest 用假读取函数覆盖四种状态，以及首样本不设置字段、失败沿用、disabled 不设置
  > 完成：`Collector.RunqStatesMapToOptionalFields`、`Collector.RunqFailureWithoutHistoryAndDisabled`。
- [x] 3.4 `worker/main.cpp` 支持 `runq: auto|on|off`，非法值报错并包含 `runq`，启动日志写明是否启用；更新 `config/worker.yaml`、`install-worker.sh` 生成的配置模板，`RUNTIME_PKGS` 加 `libbpf1`。验证：macOS 上 `runq: bogus` 报错退出，`runq: on` 以退出码 1 退出，`runq: auto` 打警告后正常运行到 /proc 检查
  > 完成：macOS 上 `runq: bogus` 报错退出；on 与 auto 的失败路径在 macOS 上会先卡在 /proc 检查，所以改在虚拟机上验证（见 5.2）。
- [x] 3.5 实现 `worker/bench/runq_compare.cpp`（`--seconds N`，输出 JSON：桶差分、N、drops、P50、P99）和 `bpf/test/compare_runqlat.py`（解析 runqlat-bpfcc 输出，计算总变差距离与 P50/P99 并判定）。验证：脚本自带用例用一份手写的 runqlat 输出和 JSON 跑通（`python3 bpf/test/compare_runqlat.py --self-test`）
  > 完成：`compare_runqlat.py --self-test` PASS。

## 4. Manager 写入（manager）

- [x] 4.1 `RedisWriter` 在样本有调度延迟时向 `node:{id}` 写入三个字段，没有时在同一个 pipeline 里 HDEL。验证：【Linux 容器】Redis 集成测试覆盖写入、值为 0 也写入、未设置时字段不存在、先设置后未设置被删除；JSON 中有和无三个字段两种情况
  > 完成：在容器里配临时 Redis 7.2，`RedisWriterTest.RunqFieldsWrittenZeroKeptAndClearedWhenAbsent` 等 4 项通过。

## 5. 部署与 Linux 验收（deploy）

- [x] 5.1 容器内完整构建（eBPF 启用）并跑全部测试，再跑 TSan 构建。验证：ctest 全部通过；Worker 二进制 `ldd` 能看到 libbpf
  > 完成：Linux 普通构建 70 项全部通过（另有 4 项按设计跳过），TSan 构建 49 项通过；`ldd worker` 有 libbpf.so.1。
- [x] 5.2 【Linux】用 `install-worker.sh` 升级 lm-worker-1（装 libbpf1）。systemd 下日志显示调度延迟采集已启用，`--dry-run` 从第二条样本起三个字段都被设置；以普通用户（无 CAP_BPF）运行时 auto 打警告、on 退出；同时升级 Manager 容器后，`HGET node:lm-worker-1 runq_p99_us` 每秒更新。验证：记录日志和数值
  > 完成：`install-worker.sh` 升级后，systemd（lm 用户）日志显示“调度延迟采集：已启用（eBPF runqlat）”；`--dry-run` 首条样本即带三个字段。以 ubuntu 用户运行时，auto 警告“load 失败：Operation not permitted”，样本不含字段；on 以退出码 1 退出。Manager 镜像重建后，`node:lm-worker-1` 的 runq 三个字段每秒更新；写入 `runq: off` 重启后字段被删除，恢复后重新出现。
- [x] 5.3 【Linux】拥挤验收：记录空闲时 60 秒的 P99；`taskset -c 1 stress -c 8` 下 P99 不低于 1000 微秒，且不低于空闲时的 10 倍。验证：记录两组数值
  > 完成：空闲 60 秒 P99 中位数 87.3 µs（50–641）；`taskset -c 1 stress -c 8` 下 30 秒 P99 中位数 22483 µs（22387–24851），约为空闲时的 258 倍。
- [x] 5.4 【Linux】与 bcc 对照：同一负载下 `runqlat-bpfcc 30 1` 与 `runq_compare --seconds 30` 同时运行，`compare_runqlat.py` 判定总变差距离 ≤ 0.1，P50、P99 相差 ≤ 2 倍。空闲负载下再跑一次作参考。验证：记录脚本输出
  > 完成：负载下总变差距离 0.040，P50 3.8 vs 4.0 µs（比值 1.04），P99 32213 vs 32275 µs（比值 1.00），PASS；空闲时总变差距离 0.036，P50 比值 1.03，P99 比值 1.06，PASS。两次丢弃数都是 0。
- [x] 5.5 【Linux】开销：空闲 60 秒内 Worker 进程 CPU 时间 ≤ 1.2 秒（读 `/proc/<pid>/stat` 的 utime+stime）；`runq_compare` 输出单次读取耗时 ≤ 1 毫秒；`perf bench sched pipe` 在 Worker 开启与关闭调度延迟采集时各跑一次，记录差异作参考。验证：记录数值
  > 完成：空闲 60 秒 Worker CPU 时间 0.16 秒；单次读取 P50 11–18 µs，最大 42 µs；`perf bench sched pipe` 关闭时 20.54 µs/op，开启时 21.65 µs/op（+5.4%，仅作参考）。
- [x] 5.6 更新 `README.md`（目录、构建依赖、里程碑表）和 `deploy/README.md`（BTF 和能力要求、`runq` 配置、回滚）。验证：按 README 在干净容器里配置能看到 eBPF 已启用
  > 完成：README（目录、依赖、CMake 选项、runq 说明、里程碑）和 deploy/README 第 6 节已更新。

## Workflow follow-up

- 归档顺序：M1（`m1-skeleton`）归档后再归档本变更，因为 `manager-hot-storage` 的增量依赖 M1 引入的能力。
- 提交前先问用户。
