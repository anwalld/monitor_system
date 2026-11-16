# Design

## Context

见 proposal.md 的 Why。M1 留下的接口和约束：

- **`CpuSource` 接口**：`CpuSource::read(CpuSnapshot&)` 返回每核 `{cpu, idle, total}` 两个累计量，`CpuBusyCalculator` 只用差值计算，单位由来源决定。共享页来源取 `idle = idle_us + iowait_us`、`total = stamp_us` 即可直接套用。
- **采集线程**：`Collector` 按绝对时间每秒采一次，CPU 读取失败时沿用上次的值。
- **目标机**：Ubuntu 24.04，内核 6.8.0-149 aarch64，4 KB 页，`CONFIG_NO_HZ_COMMON=y`。内核头文件已装，Secure Boot 关闭，不强制模块签名。
- **kmemleak**：Ubuntu 默认内核没有开 `CONFIG_DEBUG_KMEMLEAK`。
- **开发机**：macOS 不能编译内核模块，也没有 `/dev/cpustat`。用户态代码必须仍能在 macOS 上编译并跑单元测试。

## Goals / Non-Goals

**Goals:**
- 模块代码对齐开发文档给出的 `refresh` / `on_timer` 骨架，补齐 mmap、ioctl、卸载路径。
- seqlock 读端放进 `common/`，与平台无关，可以在 macOS 上用多线程单测覆盖。
- Worker 只换 CPU 来源，`Collector`、`Reporter`、上报协议都不改接口。

**Non-Goals:**
- 不做 DKMS，也不处理内核升级后自动重编。安装脚本按当前内核编一次。
- 不在 M2 接入急救。只提供刷新周期守卫和测试，M6 再用。
- 共享页不承载 CPU 以外的指标。

## Decisions

### 1. 目录与构建
```
kmod/cpustat/cpustat_uapi.h   内核和用户态共用
kmod/cpustat/cpustat.c
kmod/cpustat/Makefile         Kbuild：make -C /lib/modules/$(uname -r)/build M=$(PWD)
common/include/lm/seqlock_reader.h  模板 + 针对 cpustat_shm 的读取函数
worker/collector/shm_cpu_source.{h,cpp}
worker/collector/interval_guard.{h,cpp}
worker/bench/cpu_read_bench.cpp     可执行 cpu_read_bench
kmod/cpustat/test/                  用户态测试程序（mmap、权限、ioctl、崩溃恢复），只在 Linux 上编
```
- 内核模块不进 CMake，保持 Kbuild 独立。
- 顶层 CMake 把 `kmod/cpustat` 加进 `lm_common` 的 include 路径，Worker 就能 `#include "cpustat_uapi.h"`。
- 备选方案：用 CMake 的 `add_custom_target` 包一层 Kbuild。macOS 上会失败，需要再加平台判断，收益小，不采用。

### 2. uapi 头文件的可移植性
- 内核里（`__KERNEL__`）和 Linux 用户态包含 `<linux/types.h>`、`<linux/ioctl.h>`。
- 其他平台用 `<stdint.h>` 定义 `__u32`、`__u64`，不定义 ioctl 号。
- 头文件用 `_Static_assert`（C）或 `static_assert`（C++）钉住 `sizeof(struct cpustat_shm) == 6160`，防止两侧布局不一致。

### 3. 模块实现要点
- **init 顺序**：
  1. NO_HZ 探测：`get_cpu_idle_time_us(0, NULL)` 返回 `-1ULL` 就失败。
  2. `vmalloc_user(PAGE_ALIGN(sizeof shm))`，已清零。
  3. 填 `interval_us`。
  4. 启动 hrtimer（`CLOCK_MONOTONIC`、`HRTIMER_MODE_REL_SOFT`），先同步执行一次 `refresh()`，确保打开设备时就有数据。
  5. 最后 `misc_register`。

  任何一步失败都按相反顺序回滚。
- **exit 顺序**：`misc_deregister`、`hrtimer_cancel`、`vfree`。有 `.owner = THIS_MODULE`，并且 vma 持有 file 引用，只要还有映射或打开的 fd，模块引用计数就不为 0，rmmod 会失败，exit 时不可能还有映射者。
- **mmap**：
  - 偏移非 0 或长度超过对齐后的大小，返回 `-EINVAL`。
  - 带 `VM_WRITE` 返回 `-EACCES`。
  - 清掉 `VM_MAYWRITE`，让 mprotect 也加不上写权限。
  - 用 `vm_flags_clear()`（6.3 起的接口）配合 `remap_vmalloc_range`。
- **ioctl 与周期**：
  - `interval_ns` 用 `WRITE_ONCE` / `READ_ONCE` 读写，下一次 `hrtimer_forward_now` 生效。
  - 共享页的 `interval_us` 只由 `refresh()` 写，保持定时器是共享页唯一写者，不需要写端锁。所以 ioctl 后最多一个旧周期内，`interval_us` 才会变成新值。
  - 已经设得很长时（比如 1 秒），等上一个周期才生效会让急救晚 1 秒开始。所以 ioctl 改完后调用 `hrtimer_start(..., 0, REL_SOFT)` 立即重排一次。这个调用在进程上下文执行，与软中断回调由 hrtimer 内部的锁串行化。
- **改过周期的 fd 关闭时恢复默认**：
  - `file->private_data` 记一个标志，ioctl 成功后置位。`release` 时如果标志被置位，就把周期恢复为加载时的默认值。
  - 进程被 `kill -9` 时内核会关闭它所有的 fd，所以崩溃后也会恢复。
  - 多个进程同时改周期时以最后一次为准，任意一个改过周期的 fd 关闭都会恢复默认。目前只有 Worker 一个使用者，这一点在 uapi 头文件的注释里写明。
- **ioctl 权限**：能打开设备就能改周期，开放给 `lm` 组，依靠设备节点 0440 来控制谁能打开。
  - 备选方案：要求 `CAP_SYS_ADMIN`。Worker 的 systemd 单元没有这个能力，加上它又过宽，不采用。
- **离线 CPU**：`for_each_online_cpu` 跳过离线核，记录保持旧值。`ncpu = min(nr_cpu_ids, 256)`。

### 4. seqlock 读端
- 按开发文档实现。读者拷到调用方提供的栈上缓冲（`cpustat_core[256]`），再转成 `CpuSnapshot`。
- 提供泛型模板 `seqlock_read(const volatile uint32_t* seq, F copy)`，便于单测用自定义结构验证“两半一致”。`cpustat_shm` 的读取是它的一个实例。
- 并发 memcpy 在 C++ 内存模型下算数据竞争，这是 seqlock 的通用做法：靠前后两次读 seq 加 acquire fence 丢弃撕裂的结果。
- TSan 构建下，生成的抑制文件加一条 `race:lm::seqlock_read`，只匹配这个函数的栈帧，读端单测仍在 TSan 下运行。

### 5. ShmCpuSource 与“同一次刷新”
- **构造**：`open("/dev/cpustat", O_RDONLY | O_CLOEXEC)`，再 `mmap(PROT_READ, MAP_SHARED)`，失败抛异常并附带 errno 描述，由 main 决定回退还是退出。
- **read**：调用 seqlock 读端，输出 `ncpu` 条 `{cpu=i, idle=idle_us+iowait_us, total=stamp_us}`。
- **同一次刷新**：
  - 模块默认 1 秒刷新，采集也是 1 秒，两者相位互相独立，抖动时相邻两次采集可能读到同一次刷新。
  - `CpuBusyCalculator` 加判断：所有核 `total` 都没变时返回“无新数据”，不更新基线，`Collector` 沿用上一次的忙碌度生成样本。这样样本数不受影响，下一次按 2 秒窗口计算，平均值仍然正确。
  - 代价是读数最多滞后一个刷新周期。
  - 备选方案一：模块默认 100 毫秒刷新，滞后不超过 100 毫秒，开销也可以忽略，但偏离开发文档的默认值。
  - 备选方案二：Worker 启动时用 ioctl 改周期。会和守卫的“恢复默认”语义纠缠。
  - 先按开发文档的 1 秒实现。如果 M5/M10 测反应时间时发现滞后有影响，只需在 `/etc/modprobe.d` 改模块参数，不用改代码（见 Open Questions）。
- **`/proc` 来源**：`ProcCpuSource` 不受影响。jiffies 计时下，`total` 不变只会出现在极端情况，同样按“无新数据”处理。

### 6. 来源选择
- `main.cpp` 读 `cpu_source`：
  - `auto`：先尝试 `ShmCpuSource`，失败就记一条警告（附带原因，例如 `/dev/cpustat: No such file or directory`），改用 `ProcCpuSource`。
  - `shm`：失败直接退出，退出码 1。
  - `proc`：直接用 `ProcCpuSource`。
- 运行期间不切换来源。模块被卸载的同时 Worker 还映射着是不可能的（rmmod 会失败）。
- 来源对象把底层实现暴露给守卫：`ShmCpuSource::fd()`。守卫拿到 `CpuSource*` 后用 `dynamic_cast` 判断，不是共享页来源就什么也不做。

### 7. 刷新周期守卫
- `IntervalGuard(ShmCpuSource*, uint32_t us)`：
  - 构造时读共享页的 `interval_us` 作为旧值，再 ioctl 设成新值。
  - 析构时 ioctl 恢复旧值。
  - 失败只记警告，不抛异常。
- 守卫用的是来源对象那个 fd。Worker 进程崩溃时，内核的 release 逻辑兜底恢复默认。

### 8. 读取耗时基准
- `cpu_read_bench [--iters 100000] [--warmup 1000] [--cpu 1]`：
  - 用 `sched_setaffinity` 绑核。
  - 先测 1000 次空计时，取中位数作为计时开销。
  - 两种来源各自预热、测量，每次读取后要转成 `CpuSnapshot`，与 Worker 的实际路径一致。
  - 扣除计时开销后输出 P50、P99 和比值。
- `/proc` 组直接复用 `ProcCpuSource`，它本身就是常开 fd、`pread`、只解析 `cpuN` 行，符合开发文档对对照组的要求。

### 9. 部署
- `deploy/udev/99-cpustat.rules`：`KERNEL=="cpustat", GROUP="lm", MODE="0440"`。
- `install-worker.sh` 新增的步骤：
  1. 装 `build-essential` 和 `linux-headers-$(uname -r)`。
  2. 把 `kmod/cpustat` 拷到目标机的 `/opt/lm/kmod/cpustat`，在目标机上 make。
  3. 把编出的模块装到 `/lib/modules/$(uname -r)/extra/`，执行 `depmod`。
  4. 写 `/etc/modules-load.d/cpustat.conf`，开机自动加载。
  5. 装 udev 规则，执行 `modprobe cpustat`。
- `lm-worker.service` 加 `After=systemd-modules-load.service`。
- `deploy/check-worker-host.sh` 新增一项 headers 检查：`/lib/modules/$(uname -r)/build` 存在。

### 10. 内存泄漏验证
- Ubuntu 默认内核没有 kmemleak。验收按规格分两层：
  1. 默认内核：循环 1000 次 `insmod`/`rmmod`，每次之后检查 `/proc/vmallocinfo` 里没有 `cpustat` 的条目，比较循环前后的 `VmallocUsed` 和 `Slab`。
  2. kmemleak（可选）：用户愿意装调试内核时，再跑一遍 `echo scan > /sys/kernel/debug/kmemleak`。
- 模块本身只有一次 `vmalloc_user`，泄漏面很小，第一层足以发现这一次分配没有释放。

### 11. 实现中补充的决定（2026-11-13）
- **恢复默认周期的时机是“打开文件最后释放”**：mmap 会持有对应打开文件的引用，所以同一个 fd 既做映射又改过周期时，单独 close 并不会触发 release，要等映射也解除，或者进程退出。Worker 的情况正是这样：映射和 ioctl 共用一个 fd，崩溃时进程退出会全部释放，兜底依然成立。规格措辞已改成“打开文件最后释放时”。测试程序改用单独的 fd 来验证 close 的行为。
- **基准改为分批计时**：虚拟机的 arm64 计时器一跳约 41 ns，比一次共享页读取（约 12 ns）还长，逐次计时只能得到 0 或 1 ns 这种量化值。改为每 100 次读取计一次时，两种来源用同一方法，`--batch 1` 仍可退回逐次计时。
- **Worker 主机自检增加 headers 项**（`/lib/modules/$(uname -r)/build`），这是对 M0 自检的扩展。
- **`lm-worker.service` 只加 `After=systemd-modules-load.service`**：模块开机由 modules-load 加载，udev 规则随设备出现时生效。Worker 即使比设备节点先启动也只会回退到 /proc，不会失败。
- **编译器提示**：Kbuild 提示“compiler differs from the one used to build the kernel”。内核是 `aarch64-linux-gnu-gcc-13` 编的，本机是 `gcc-13`，两者是同一版本（13.3.0），只是名字不同，可以忽略。

## 性能与资源影响

- **采集开销**：
  - 共享页读取是一次约 6 KB 内的 memcpy 加两次原子读。基准目标是不超过 `/proc` 路径的 15%。
  - 模块每次刷新对每个在线核调两次内核函数。1 秒周期下开销可以忽略。1 毫秒周期（4 核）约每秒 8000 次调用，只在急救期间出现。
- 上报频率、Redis/MySQL 写入量、Manager 锁都不变。

## Risks / Trade-offs

- [hrtimer 回调调用的 `get_cpu_*_time_us` 内部取锁，在软中断上下文是否安全] → 这两个函数只读 per-cpu 的 tick_sched 并用 seqcount 保护，内核里有在软中断中调用的先例。在虚拟机上开 `CONFIG_PROVE_LOCKING` 不现实，改为 1 毫秒周期跑 10 分钟，确认 dmesg 没有警告。
- [多个进程改周期互相覆盖] → 当前只有 Worker 使用。在头文件注释和部署文档里说明。
- [读数最多滞后一个刷新周期] → 见决定 5。可以通过模块参数调整，不改代码。
- [内核升级后模块版本不匹配，加载失败] → Worker 在 `auto` 模式下会回退到 `/proc` 继续工作，日志里有警告。重新运行 `install-worker.sh` 即可重编。
- [Secure Boot 主机拒绝加载未签名模块] → 目标虚拟机关闭了 Secure Boot。部署文档说明开启时需要用 MOK 签名或改用 `/proc` 来源。
- [aarch64 和 x86_64 差异] → 模块只用通用 API。目前只在 aarch64 虚拟机上验证，x86_64 未验证，如实标注。

## Migration Plan

1. 先在 Worker 机上用 `install-worker.sh` 编译并加载模块，再更新 Worker 二进制。顺序反过来也可以：`auto` 模式下没有模块会回退到 `/proc`。
2. 回滚：`modprobe -r cpustat`，删除 `/etc/modules-load.d/cpustat.conf`，Worker 自动回退到 `/proc`。也可以在配置里写 `cpu_source: proc`。

## Open Questions

- 默认刷新周期是否从 1 秒改为 100 毫秒以减少读数滞后：等 M5/M10 测出调度反应时间后再定。只改模块参数，不影响规格结构和任务划分。
