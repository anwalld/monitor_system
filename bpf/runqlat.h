/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * runqlat 内核程序与 Worker 用户态共用的常量和分桶函数。
 * 只用基本整数类型，不依赖 vmlinux.h 或 <stdint.h>，BPF C 和 C++ 都能包含。
 *
 * 对数线性分桶（微秒）：
 *   v < 8         → 桶 v
 *   v >= 8        → k = floor(log2 v)，桶 (k-2)*8 + ((v >> (k-3)) & 7)，即每个 2 的幂区间再分 8 份
 *   桶号 > 207    → 207（约 2^28 微秒，268 秒以上）
 */
#ifndef LM_RUNQLAT_H
#define LM_RUNQLAT_H

#define RUNQ_BUCKETS 208
#define RUNQ_LINEAR 8          /* 前 8 个桶是 0..7 微秒，每桶 1 微秒 */
#define RUNQ_SUB_SHIFT 3       /* 每个 2 的幂区间分 2^3 = 8 份 */
#define RUNQ_START_ENTRIES 10240

#if defined(__bpf__)
#define RUNQ_INLINE static __always_inline
#else
#define RUNQ_INLINE static inline
#endif

/* 无循环求 floor(log2 v)，v > 0（与 bcc 的 log2/log2l 写法相同，BPF 里不依赖 clz） */
RUNQ_INLINE unsigned int runq_log2_u32(unsigned int v)
{
	unsigned int r, shift;

	r = (v > 0xFFFF) << 4;
	v >>= r;
	shift = (v > 0xFF) << 3;
	v >>= shift;
	r |= shift;
	shift = (v > 0xF) << 2;
	v >>= shift;
	r |= shift;
	shift = (v > 0x3) << 1;
	v >>= shift;
	r |= shift;
	r |= (v >> 1);
	return r;
}

RUNQ_INLINE unsigned int runq_log2_u64(unsigned long long v)
{
	unsigned int hi = (unsigned int)(v >> 32);

	if (hi)
		return runq_log2_u32(hi) + 32;
	return runq_log2_u32((unsigned int)v);
}

RUNQ_INLINE unsigned int runq_bucket_of(unsigned long long us)
{
	unsigned int k, b;

	if (us < RUNQ_LINEAR)
		return (unsigned int)us;
	k = runq_log2_u64(us);
	b = (k - 2) * 8 + (unsigned int)((us >> (k - RUNQ_SUB_SHIFT)) & 7);
	return b < RUNQ_BUCKETS ? b : RUNQ_BUCKETS - 1;
}

/* 桶 b 的区间下界与宽度（微秒），供用户态插值 */
RUNQ_INLINE unsigned long long runq_bucket_low(unsigned int b)
{
	unsigned int k, s;

	if (b < RUNQ_LINEAR)
		return b;
	k = b / 8 + 2;
	s = b % 8;
	return (unsigned long long)(8 + s) << (k - RUNQ_SUB_SHIFT);
}

RUNQ_INLINE unsigned long long runq_bucket_width(unsigned int b)
{
	if (b < RUNQ_LINEAR)
		return 1;
	return 1ULL << (b / 8 + 2 - RUNQ_SUB_SHIFT);
}

#endif /* LM_RUNQLAT_H */
