# Spec Delta

## Purpose

cpustat 内核模块定时把每核累计空闲时间写进一块只读共享页，用户态进程 mmap 后无需系统调用、无需解析文本就能取得一致的 CPU 读数。

## ADDED Requirements

### Requirement: 加载前置条件
模块加载时 SHALL 检查内核能否给出空闲时间（NO_HZ 开启）。不能时 MUST 加载失败、返回错误，并在内核日志中打印一行原因，不留下设备节点。模块许可证 SHALL 为 GPL。

#### Scenario: NO_HZ 开启
- **WHEN** 在 Ubuntu 24.04（内核 6.8，`CONFIG_NO_HZ_COMMON=y`）上执行 `insmod cpustat.ko`
- **THEN** 加载成功，出现 `/dev/cpustat`，内核日志中有一行包含当前刷新周期的加载信息

#### Scenario: 无法取得空闲时间
- **WHEN** 内核取空闲时间的接口返回失败
- **THEN** `insmod` 失败，`/dev/cpustat` 不存在，内核日志中有一行说明需要 NO_HZ

### Requirement: 设备节点与权限
模块 SHALL 注册字符设备 `/dev/cpustat`。设备 SHALL 支持只读 mmap，MUST 拒绝任何可写映射，已建立的映射也 MUST NOT 能通过 mprotect 改成可写。映射长度超过共享页大小或偏移量不为 0 时 MUST 返回 EINVAL。设备文件的访问权限由部署时的 udev 规则设为 root 与 `lm` 组可读（0440）。

#### Scenario: 只读映射
- **WHEN** 进程以只读方式打开 `/dev/cpustat` 并以 `PROT_READ`、`MAP_SHARED` 映射共享页
- **THEN** 映射成功，能读到 `ncpu` 等于在线 CPU 编号上限（不超过 256）

#### Scenario: 可写映射被拒绝
- **WHEN** 进程以 `PROT_READ | PROT_WRITE` 映射，或映射成功后调用 `mprotect` 加上 `PROT_WRITE`
- **THEN** 调用失败，返回 EACCES 或 EPERM，共享页内容不变

#### Scenario: 越界映射
- **WHEN** 映射长度大于共享页按页对齐后的大小，或偏移量非 0
- **THEN** mmap 返回 EINVAL

### Requirement: 共享页布局
共享页布局 SHALL 与开发文档的 `cpustat_uapi.h` 一致：头部依次为 `seq`、`ncpu`、`interval_us`、`reserved` 四个 32 位字段，其后是 256 个每核记录，每条依次为 `idle_us`、`iowait_us`、`stamp_us` 三个 64 位字段。`idle_us`、`iowait_us` 是内核累计值（微秒），`stamp_us` 是写入该核记录时取的单调时间戳（微秒）。该头文件 MUST 能同时被内核模块和用户态 C/C++ 代码包含。

#### Scenario: 用户态与内核一致
- **WHEN** 用户态程序包含 `cpustat_uapi.h` 编译
- **THEN** `sizeof(struct cpustat_shm)` 等于 16 + 256 × 24 = 6160 字节，`cpu` 数组的偏移为 16

### Requirement: seq 奇偶写协议
模块 SHALL 是共享页唯一的写者。每次刷新 MUST 先把 `seq` 加 1 变为奇数，写完全部在线 CPU 的记录和 `ncpu` 后再加 1 变为偶数，前后各有写内存屏障。读者看到 `seq` 为偶数且读取前后 `seq` 相同时，读到的就是一次完整刷新的结果。

#### Scenario: 刷新期间 seq 递增
- **WHEN** 模块以 1 秒周期运行 10 秒
- **THEN** 用户态观察到的偶数 `seq` 每秒增加 2，各核 `stamp_us` 每次刷新都变大

#### Scenario: 离线 CPU
- **WHEN** 某个 CPU 处于离线状态
- **THEN** 该核记录保持上一次的值不再更新，其余核照常刷新

### Requirement: 刷新周期与 ioctl
默认刷新周期 SHALL 为 1 秒，加载时可用模块参数 `interval_us` 修改（不小于 1000）。刷新 SHALL 在软中断上下文执行，不占用硬中断时间。ioctl `CPUSTAT_SET_INTERVAL`（参数为微秒）SHALL 修改刷新周期，下一次刷新起生效，并同步更新共享页的 `interval_us`；参数小于 1000 时 MUST 返回 EINVAL 且周期不变。通过 ioctl 改过周期的打开文件被最后释放时（描述符全部关闭且没有映射引用它，包括进程退出），周期 SHALL 恢复为加载时的默认值，避免进程异常退出后一直以 1 毫秒刷新。

#### Scenario: 改为 1 毫秒
- **WHEN** 进程对 `/dev/cpustat` 调用 `CPUSTAT_SET_INTERVAL(1000)`
- **THEN** 共享页 `interval_us` 变为 1000，偶数 `seq` 每秒增加约 2000（误差 10% 以内）

#### Scenario: 参数过小
- **WHEN** 调用 `CPUSTAT_SET_INTERVAL(500)`
- **THEN** 返回 EINVAL，`interval_us` 保持原值

#### Scenario: 改周期的进程崩溃
- **WHEN** 进程把周期改为 1000 微秒后被 `kill -9`
- **THEN** 共享页 `interval_us` 恢复为 1000000

### Requirement: 卸载安全
只要还有进程映射着共享页或持有打开的设备，`rmmod cpustat` MUST 失败（模块正在使用）。卸载 SHALL 先注销设备、停止定时器，再释放共享页，MUST NOT 泄漏内存。

#### Scenario: 映射期间卸载
- **WHEN** 一个进程映射着共享页时执行 `rmmod cpustat`
- **THEN** rmmod 失败并提示模块正在使用，该进程继续读到正常刷新的数据

#### Scenario: 反复加载卸载
- **WHEN** 循环执行 `insmod` / `rmmod` 1000 次
- **THEN** 每次都成功；结束后 `/proc/vmallocinfo` 中没有本模块的分配残留，`VmallocUsed` 与循环前相比增长不超过 64 KB；内核开启 kmemleak 时扫描结果中没有本模块的泄漏
