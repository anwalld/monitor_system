// SPDX-License-Identifier: GPL-2.0
/*
 * cpustat：定时把每核累计空闲时间写进一块只读共享页，用户态 mmap /dev/cpustat 后
 * 按 seq 协议直接读取，不用系统调用、不用解析 /proc/stat 文本。
 *
 * - 共享页：vmalloc_user 分配，remap_vmalloc_range 映射，只允许只读映射。
 * - 刷新：hrtimer（HRTIMER_MODE_REL_SOFT，回调在软中断里），定时器是共享页唯一写者。
 * - 周期：模块参数 interval_us（默认 1 秒），ioctl CPUSTAT_SET_INTERVAL 运行时修改；
 *   改过周期的文件描述符关闭时恢复默认，防止进程崩溃后一直以 1 毫秒刷新。
 */

#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/tick.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "cpustat_uapi.h"

static unsigned int interval_us = CPUSTAT_DEFAULT_INTERVAL_US;
module_param(interval_us, uint, 0444);
MODULE_PARM_DESC(interval_us, "default refresh interval in microseconds (>= 1000)");

static struct cpustat_shm *shm;
static size_t shm_size;
static u64 default_interval_ns;
static u64 interval_ns;
static struct hrtimer timer;

/* 只在 init（定时器启动前）和定时器回调里调用；同一个 hrtimer 的回调不会并发执行。 */
static void refresh(void)
{
	int cpu;

	WRITE_ONCE(shm->seq, shm->seq + 1); /* 变奇数：开始写 */
	smp_wmb();
	for_each_online_cpu(cpu) {
		struct cpustat_core *c;
		u64 t = 0;

		if (cpu >= CPUSTAT_MAX_CPU)
			break;
		c = &shm->cpu[cpu];
		c->idle_us = get_cpu_idle_time_us(cpu, NULL);
		c->iowait_us = get_cpu_iowait_time_us(cpu, &t);
		c->stamp_us = t;
	}
	shm->ncpu = min_t(u32, nr_cpu_ids, CPUSTAT_MAX_CPU);
	shm->interval_us = (u32)div_u64(READ_ONCE(interval_ns), NSEC_PER_USEC);
	smp_wmb();
	WRITE_ONCE(shm->seq, shm->seq + 1); /* 变偶数：写完 */
}

static enum hrtimer_restart on_timer(struct hrtimer *t)
{
	refresh();
	hrtimer_forward_now(t, ns_to_ktime(READ_ONCE(interval_ns)));
	return HRTIMER_RESTART;
}

/* 改周期后立即触发一次刷新，让新周期马上生效（否则要等完旧周期，急救会晚开始最多 1 秒）。 */
static void set_interval_ns(u64 ns)
{
	WRITE_ONCE(interval_ns, ns);
	hrtimer_start(&timer, 0, HRTIMER_MODE_REL_SOFT);
}

static int cpustat_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL; /* 非 NULL 表示这个 fd 改过周期 */
	return 0;
}

static int cpustat_release(struct inode *inode, struct file *file)
{
	if (file->private_data)
		set_interval_ns(default_interval_ns);
	return 0;
}

static int cpustat_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff != 0 || len > shm_size)
		return -EINVAL;
	if (vma->vm_flags & VM_WRITE)
		return -EACCES;
	vm_flags_clear(vma, VM_MAYWRITE); /* mprotect 也加不上写权限 */
	return remap_vmalloc_range(vma, shm, 0);
}

static long cpustat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	u32 us;

	if (cmd != CPUSTAT_SET_INTERVAL)
		return -ENOTTY;
	if (get_user(us, (u32 __user *)arg))
		return -EFAULT;
	if (us < CPUSTAT_MIN_INTERVAL_US)
		return -EINVAL;
	file->private_data = (void *)1UL;
	set_interval_ns((u64)us * NSEC_PER_USEC);
	return 0;
}

static const struct file_operations cpustat_fops = {
	.owner = THIS_MODULE, /* 有进程打开或映射着时模块卸不掉 */
	.open = cpustat_open,
	.release = cpustat_release,
	.mmap = cpustat_mmap,
	.unlocked_ioctl = cpustat_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice cpustat_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cpustat",
	.fops = &cpustat_fops,
	.mode = 0440, /* 组由 udev 规则改为 lm */
};

static int __init cpustat_init(void)
{
	int ret;

	if (get_cpu_idle_time_us(0, NULL) == (u64)-1) {
		pr_err("cpustat: idle time unavailable, kernel needs NO_HZ (CONFIG_NO_HZ_COMMON=y, no nohz=off)\n");
		return -ENODEV;
	}
	if (interval_us < CPUSTAT_MIN_INTERVAL_US) {
		pr_err("cpustat: interval_us=%u is below %u\n", interval_us, CPUSTAT_MIN_INTERVAL_US);
		return -EINVAL;
	}

	shm_size = PAGE_ALIGN(sizeof(*shm));
	shm = vmalloc_user(shm_size); /* 已清零 */
	if (!shm)
		return -ENOMEM;

	default_interval_ns = (u64)interval_us * NSEC_PER_USEC;
	interval_ns = default_interval_ns;
	refresh(); /* 设备出现时就有数据 */

	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	timer.function = on_timer;
	hrtimer_start(&timer, ns_to_ktime(interval_ns), HRTIMER_MODE_REL_SOFT);

	ret = misc_register(&cpustat_dev);
	if (ret) {
		hrtimer_cancel(&timer);
		vfree(shm);
		shm = NULL;
		return ret;
	}
	pr_info("cpustat: loaded, ncpu=%u interval=%uus shm=%zu bytes\n", shm->ncpu, interval_us,
		shm_size);
	return 0;
}

static void __exit cpustat_exit(void)
{
	misc_deregister(&cpustat_dev);
	hrtimer_cancel(&timer);
	vfree(shm);
	shm = NULL;
	pr_info("cpustat: unloaded\n");
}

module_init(cpustat_init);
module_exit(cpustat_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("anwalld");
MODULE_DESCRIPTION("Per-CPU idle time exported through a read-only shared page");
