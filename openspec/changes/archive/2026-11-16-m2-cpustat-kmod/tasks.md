# Tasks

## 1. 共享页契约与 seqlock 读端（common）

- [x] 1.1 新增 `kmod/cpustat/cpustat_uapi.h`：结构体、ioctl 号与开发文档一致；非 Linux 平台用 `<stdint.h>` 提供 `__u32`/`__u64`；加 `sizeof == 6160` 静态断言；注释说明多进程改周期以最后一次为准。顶层 CMake 把 `kmod/cpustat` 加进 `lm_common` 的 include 路径。验证：macOS 上 `cmake --build build` 成功，gtest 断言 `offsetof(cpustat_shm, cpu) == 16`
  > 2026-11-13 完成：macOS 与 Linux 容器构建通过，`CpustatUapi.Layout` 用例断言布局。
- [x] 1.2 实现 `common/include/lm/seqlock_reader.h`：泛型 `seqlock_read`（最多重试 100 次）和 `read_cpustat(const cpustat_shm*, cpustat_core* buf, uint32_t& ncpu)`，`ncpu` 截断到 256。验证：gtest 覆盖正常读、seq 停在奇数时返回失败、ncpu=1000 时截断
  > 完成：`Seqlock.*` 用例覆盖正常读、奇数 seq 失败、ncpu 截断。
- [x] 1.3 编写 seqlock 并发测试：1 个写线程按奇偶协议把前后两半写成同一递增值，4 个读线程各读 10 万次，校验两半一致。TSan 抑制文件加一条 `race:lm::seqlock_read`。验证：普通构建与 `-DLM_ENABLE_TSAN=ON` 构建下 ctest 都通过
  > 完成：测试里的写线程每次写完留一小段空档（真实写者是周期刷新），否则读者 100 次重试都会撞上写入。macOS 连跑 20 次通过，Linux 普通构建和 TSan 构建（38 项）都通过，没有撕裂。

## 2. 内核模块 cpustat（kmod）

- [x] 2.1 编写 `kmod/cpustat/cpustat.c` 的 init/exit：NO_HZ 探测、`vmalloc_user`、模块参数 `interval_us`（默认 1000000，不小于 1000）、首次同步 `refresh()`、软中断 hrtimer、`misc_register`、失败时按相反顺序回滚；exit 依次注销设备、取消定时器、释放共享页。新增 Kbuild `Makefile`。验证：【Linux】`make -C kmod/cpustat` 无警告，`insmod` 后出现 `/dev/cpustat`，dmesg 有加载信息，`rmmod` 成功
  > 完成（lm-worker-1，6.8.0-149 aarch64）：编译只有一条无害的编译器名称提示，`insmod` 后 dmesg 显示 `ncpu=4 interval=1000000us shm=8192 bytes`。
- [x] 2.2 按开发文档骨架实现 `refresh()` 与 `on_timer()`（seq 奇偶加屏障、跳过离线核、`ncpu = min(nr_cpu_ids, 256)`、每次写 `interval_us`）。验证：【Linux】用 2.4 的测试程序观察到偶数 seq 每秒加 2、各核 stamp_us 递增
  > 完成：默认周期下 3 秒 seq +6，stamp_us 递增。
- [x] 2.3 实现 mmap（偏移非 0 或超长返回 EINVAL，带 VM_WRITE 返回 EACCES，清除 VM_MAYWRITE，`remap_vmalloc_range`）和 ioctl `CPUSTAT_SET_INTERVAL`（小于 1000 返回 EINVAL，改完立即重排定时器，在 `file->private_data` 记标志）；release 时恢复加载时的默认周期。验证：【Linux】见 2.4
  > 完成：见 2.4。
- [x] 2.4 编写 `kmod/cpustat/test/cpustat_test.c`（用户态，只在 Linux 上编），覆盖：只读映射成功；可写映射和 mprotect 加写失败；越界映射返回 EINVAL；ioctl 1000 后 1 秒内偶数 seq 增加约 2000（±10%）；ioctl 500 返回 EINVAL；子进程改成 1000 后被 `kill -9`，`interval_us` 恢复为 1000000。验证：【Linux】`sudo ./cpustat_test` 全部 PASS
  > 完成：`sudo ./cpustat_test` 17 项全部 PASS。1 毫秒周期下 1 秒 seq +1962。首次运行有一项失败，原因是映射持有打开文件导致 close 不触发 release，已改规格措辞和测试（见 design 第 11 节）。
- [x] 2.5 卸载安全与泄漏：映射期间 `rmmod` 应失败；脚本 `kmod/cpustat/test/load_loop.sh` 循环 `insmod`/`rmmod` 1000 次，每次检查 `/proc/vmallocinfo` 无 cpustat 条目，比较前后 `VmallocUsed` 和 `Slab`（增长不超过 64 KB）。另外以 1 毫秒周期运行 10 分钟，确认 dmesg 无警告。验证：【Linux】脚本输出 PASS 和前后数值；能用 kmemleak 时附上扫描结果，不能用时如实注明
  > 完成：映射期间 rmmod 被拒绝；1000 次 insmod/rmmod 用时 60 秒，`/proc/vmallocinfo` 没有残留，`VmallocUsed` +44 KB（泄漏的话应为约 8 MB），Slab −484 KB。1 毫秒周期跑满 600 秒（约 60 万次刷新），dmesg 没有警告，进程退出后周期恢复为 1 秒。kmemleak：Ubuntu 默认内核未开启，没有做。

## 3. Worker 改读共享页（worker）

- [x] 3.1 `CpuBusyCalculator::update` 增加“无新数据”判断：所有核 `total` 都没变时返回无新数据，不更新基线；`Collector` 收到无新数据时沿用上一次的忙碌度生成样本。验证：gtest 覆盖同一快照连续两次、之后按长窗口计算；M1 原有用例仍通过
  > 完成：新增 `BusyResult` 三态，加了 `CpuBusy.SameSnapshotIsNoNewDataAndKeepsBaseline` 和 `Collector.SameCpuDataStillPublishesWithLastBusy` 两个用例。
- [x] 3.2 实现 `worker/collector/shm_cpu_source.{h,cpp}`：构造时 open + mmap，失败抛异常并带 errno 描述；`read()` 调用 `read_cpustat` 后转成 `CpuSnapshot`（idle = idle_us + iowait_us，total = stamp_us）；提供 `fd()` 和 `interval_us()`。非 Linux 平台构造直接抛异常。验证：gtest 用临时文件模拟共享页（写一份 cpustat_shm 后 mmap），校验转换结果
  > 完成：`ShmCpuSource.*` 用普通文件模拟共享页。
- [x] 3.3 实现 `worker/collector/interval_guard.{h,cpp}`：构造时记下旧周期并 ioctl 设新值，析构时恢复旧值，失败只记警告；传入的来源不是 `ShmCpuSource` 时什么也不做。验证：gtest 覆盖空操作路径；【Linux】在 2.4 测试程序里补一个用例：守卫构造后 interval_us 为 1000，析构（含异常离开）后为 1000000
  > 完成：在虚拟机上以 root 运行 `IntervalGuardLive.SetsAndRestoresIncludingOnException`，通过（放在 gtest 里，没有放进 cpustat_test）。
- [x] 3.4 `worker/main.cpp` 支持 `cpu_source: auto|shm|proc`：auto 失败时回退并警告，shm 失败时以退出码 1 退出，非法值时启动失败且错误信息包含 `cpu_source`；启动日志写明实际使用的来源。更新 `config/worker.yaml`。验证：macOS 上 `cpu_source: bogus` 时报错退出；【Linux】未加载模块时 auto 回退、shm 退出
  > 完成：macOS 上 `cpu_source: bogus` 报错并以退出码 2 退出。虚拟机上：auto 无模块时回退并警告；shm 无模块时报错并以退出码 1 退出。
- [x] 3.5 实现 `worker/bench/cpu_read_bench.cpp`：绑核、扣除计时开销、预热 1000 次后测 10 万次，输出两种来源的 P50、P99 和比值；没有 `/dev/cpustat` 时只测 /proc 组并提示。验证：macOS 上能编译；【Linux】见 4.3
  > 完成：macOS 上编译通过。

## 4. 部署与 Linux 验收（deploy）

- [x] 4.1 新增 `deploy/udev/99-cpustat.rules`（`GROUP="lm", MODE="0440"`）；`install-worker.sh` 增加装 headers 与 build-essential、拷贝并在目标机编译模块、安装到 `extra/`、`depmod`、写 `/etc/modules-load.d/cpustat.conf`、装 udev 规则、`modprobe`；`lm-worker.service` 加 `After=systemd-modules-load.service`；`check-worker-host.sh` 增加 headers 检查。验证：`bash -n` 和 shellcheck 通过；【Linux】在 lm-worker-1 上运行安装脚本后 `ls -l /dev/cpustat` 为 `cr--r----- root lm`，重启后模块自动加载
  > 完成：shellcheck 通过；安装后 `/dev/cpustat` 为 `cr--r----- root lm`；重启后模块自动加载（引用计数 1，即 Worker 的映射），Worker 读共享页。
- [x] 4.2 【Linux】Worker 以 `cpu_source: auto` 运行于 lm-worker-1：日志显示来源为共享页；`taskset -c 2 stress -c 1` 时 core_busy[2] ≥ 0.95、cpu_busy 在 0.2–0.35；`stress -c 2` 下 shm 与 proc 两组 `--dry-run` 各 30 秒，cpu_busy 平均值相差 ≤ 0.05；上报到 Manager 后 Redis 每秒更新。验证：记录各项数值
  > 完成：日志显示来源为共享页；`taskset -c 2 stress -c 1` 时 core_busy[2] 在 7 个样本中都是 1.000，cpu_busy 0.254–0.268；`stress -c 2` 下 shm 与 proc 同时各跑 30 秒，平均值分别为 0.508、0.513，相差 0.005；Redis `node:lm-worker-1` 的 seq 每秒加 1。
- [x] 4.3 【Linux】在 lm-worker-1 上运行 `cpu_read_bench`：共享页 P50 不超过 /proc/stat P50 的 15%（对应开发文档 M2 验收“读取耗时低 85% 以上”）。验证：记录两组 P50、P99 和比值
  > 完成（分批计时，batch=100，连跑 3 次）：/proc/stat P50 约 7.7–7.9 µs，共享页 P50 11.7 ns，P99 12.9 ns，比值 0.001–0.002，耗时降低 99.8%。
- [x] 4.4 更新 `deploy/README.md`（模块编译与加载、udev 权限、Secure Boot 说明、回滚方法）、顶层 `README.md` 里程碑表与构建说明。验证：按 README 在 lm-worker-1 上从卸载状态重新走一遍，结果与 4.1 一致
  > 完成：在虚拟机上按回滚步骤彻底卸载模块（Worker 回退到 /proc 并警告），再运行 `install-worker.sh`，结果与 4.1 一致；自检 headers 项 OK。

## Workflow follow-up

- M2 验收完成后提交，提交前先问用户。
- M1 的 7.1、M0 的 4.2 仍在等 3 台 Worker，与本 change 无关。
