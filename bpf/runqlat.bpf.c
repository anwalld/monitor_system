// SPDX-License-Identifier: GPL-2.0
/*
 * 调度排队延迟直方图（libbpf + CO-RE）。
 * 任务变为可运行（唤醒、新建唤醒、被换下但仍可运行）时记入队时间，被换上 CPU 时算延迟并记入对数线性直方图。
 * 用户态每秒对各核求和、与上一秒做差，得到本周期分布（见 worker/collector/runq_histogram.h）。
 */
#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "runqlat.h"

#define TASK_RUNNING 0

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, RUNQ_START_ENTRIES);
	__type(key, u32);   /* pid */
	__type(value, u64); /* 入队时间 ns */
} start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, RUNQ_BUCKETS);
	__type(key, u32);
	__type(value, u64);
} hist SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} drops SEC(".maps");

static __always_inline int mark(u32 pid)
{
	u64 ts;
	u32 z = 0;

	if (pid == 0) /* idle 线程 */
		return 0;
	ts = bpf_ktime_get_ns();
	if (bpf_map_update_elem(&start, &pid, &ts, BPF_ANY)) {
		u64 *d = bpf_map_lookup_elem(&drops, &z);

		if (d)
			(*d)++;
	}
	return 0;
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(on_wakeup, struct task_struct *p)
{
	return mark(p->pid);
}

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(on_wakeup_new, struct task_struct *p)
{
	return mark(p->pid);
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_switch, bool preempt, struct task_struct *prev, struct task_struct *next)
{
	u32 pid = next->pid, b;
	u64 *t0, *cnt, now;

	if (preempt || BPF_CORE_READ(prev, __state) == TASK_RUNNING)
		mark(prev->pid); /* 被抢占或让出后仍可运行：重新入队 */
	t0 = bpf_map_lookup_elem(&start, &pid);
	if (!t0)
		return 0;
	now = bpf_ktime_get_ns();
	b = runq_bucket_of(now > *t0 ? (now - *t0) / 1000 : 0);
	bpf_map_delete_elem(&start, &pid); /* 用完即删 */
	cnt = bpf_map_lookup_elem(&hist, &b);
	if (cnt)
		(*cnt)++;
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
