/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * cpustat 内核模块与用户态共用的共享页定义。
 *
 * 共享页由模块的 hrtimer 定时刷新，模块是唯一写者。读者按 seq 协议读取：
 *   seq 为奇数表示正在写；读前、读后 seq 相同且为偶数，读到的就是一次完整刷新。
 *
 * ioctl CPUSTAT_SET_INTERVAL 修改刷新周期（微秒，不小于 1000）。多个进程同时修改时
 * 以最后一次为准；任何一个改过周期的文件描述符关闭时，周期恢复为模块加载时的默认值。
 */
#ifndef CPUSTAT_UAPI_H
#define CPUSTAT_UAPI_H

#if defined(__KERNEL__) || defined(__linux__)
#include <linux/ioctl.h>
#include <linux/types.h>
#else
/* 非 Linux 平台（如 macOS 开发机）只用到结构体定义，不使用 ioctl */
#include <stdint.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

#define CPUSTAT_MAX_CPU 256
#define CPUSTAT_DEV_PATH "/dev/cpustat"
#define CPUSTAT_MIN_INTERVAL_US 1000u
#define CPUSTAT_DEFAULT_INTERVAL_US 1000000u

struct cpustat_core {
	__u64 idle_us;   /* get_cpu_idle_time_us 累计值 */
	__u64 iowait_us; /* get_cpu_iowait_time_us 累计值 */
	__u64 stamp_us;  /* 本次读取用的时间戳（单调时钟，微秒） */
};

struct cpustat_shm {
	__u32 seq;         /* 奇数表示正在写 */
	__u32 ncpu;
	__u32 interval_us; /* 当前刷新周期 */
	__u32 reserved;
	struct cpustat_core cpu[CPUSTAT_MAX_CPU];
};

#if defined(__KERNEL__) || defined(__linux__)
#define CPUSTAT_IOC_MAGIC 'c'
#define CPUSTAT_SET_INTERVAL _IOW(CPUSTAT_IOC_MAGIC, 1, __u32) /* 微秒，不小于 1000 */
#endif

#ifdef __cplusplus
static_assert(sizeof(struct cpustat_core) == 24, "cpustat_core layout");
static_assert(sizeof(struct cpustat_shm) == 6160, "cpustat_shm layout");
#else
_Static_assert(sizeof(struct cpustat_core) == 24, "cpustat_core layout");
_Static_assert(sizeof(struct cpustat_shm) == 6160, "cpustat_shm layout");
#endif

#endif /* CPUSTAT_UAPI_H */
