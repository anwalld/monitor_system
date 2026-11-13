// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * cpustat 模块的用户态验收测试（需要 root：部分用例以 O_RDWR 打开设备）。
 *   sudo ./cpustat_test            运行全部用例，逐项输出 PASS/FAIL，全部通过时退出码 0
 *   ./cpustat_test --hold <秒>     只读映射后保持指定秒数，用于验证映射期间 rmmod 失败
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cpustat_uapi.h"

static int failures;

#define CHECK(cond, fmt, ...)                                                   \
	do {                                                                    \
		if (cond) {                                                     \
			printf("PASS  " fmt "\n", ##__VA_ARGS__);               \
		} else {                                                        \
			printf("FAIL  " fmt "\n", ##__VA_ARGS__);               \
			failures++;                                             \
		}                                                               \
	} while (0)

static uint32_t read_seq(const volatile struct cpustat_shm *s)
{
	return __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
}

static uint32_t even_seq(const volatile struct cpustat_shm *s)
{
	uint32_t v;

	while ((v = read_seq(s)) & 1u)
		;
	return v;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static const struct cpustat_shm *map_ro(int *fd_out)
{
	int fd = open(CPUSTAT_DEV_PATH, O_RDONLY | O_CLOEXEC);
	void *p;

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", CPUSTAT_DEV_PATH, strerror(errno));
		exit(2);
	}
	p = mmap(NULL, sizeof(struct cpustat_shm), PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		exit(2);
	}
	*fd_out = fd;
	return p;
}

static int set_interval(int fd, uint32_t us)
{
	return ioctl(fd, CPUSTAT_SET_INTERVAL, &us);
}

static int hold(int seconds)
{
	int fd;
	const struct cpustat_shm *s = map_ro(&fd);

	printf("holding mapping for %d s (pid %d)\n", seconds, getpid());
	fflush(stdout);
	for (int i = 0; i < seconds; i++) {
		sleep(1);
		printf("seq=%u stamp0=%llu\n", even_seq(s), (unsigned long long)s->cpu[0].stamp_us);
		fflush(stdout);
	}
	return 0;
}

int main(int argc, char **argv)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t shm_pages = (sizeof(struct cpustat_shm) + page - 1) / page * page;
	const struct cpustat_shm *s;
	int fd, fd2, rc;
	void *p;

	if (argc == 3 && strcmp(argv[1], "--hold") == 0)
		return hold(atoi(argv[2]));

	/* 1. 只读映射 */
	s = map_ro(&fd);
	{
		long nconf = sysconf(_SC_NPROCESSORS_CONF);
		uint32_t n = s->ncpu;

		CHECK(n >= 1 && n <= CPUSTAT_MAX_CPU && (long)n >= sysconf(_SC_NPROCESSORS_ONLN),
		      "read-only mmap, ncpu=%u (configured %ld)", n, nconf);
		CHECK(s->interval_us == CPUSTAT_DEFAULT_INTERVAL_US, "default interval_us=%u",
		      s->interval_us);
	}

	/* 2. 可写映射被拒绝 */
	p = mmap(NULL, sizeof(struct cpustat_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p == MAP_FAILED && errno == EACCES, "RW mmap on O_RDONLY fd rejected (%s)",
	      p == MAP_FAILED ? strerror(errno) : "mapped!");
	fd2 = open(CPUSTAT_DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd2 >= 0) {
		p = mmap(NULL, sizeof(struct cpustat_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
		CHECK(p == MAP_FAILED && errno == EACCES, "RW mmap on O_RDWR fd rejected by module (%s)",
		      p == MAP_FAILED ? strerror(errno) : "mapped!");
		close(fd2);
	} else {
		printf("SKIP  O_RDWR open failed (%s), run as root to test module-side check\n",
		       strerror(errno));
	}
	rc = mprotect((void *)s, sizeof(struct cpustat_shm), PROT_READ | PROT_WRITE);
	CHECK(rc != 0 && (errno == EACCES || errno == EPERM), "mprotect add PROT_WRITE rejected (%s)",
	      rc ? strerror(errno) : "allowed!");

	/* 3. 越界映射 */
	p = mmap(NULL, shm_pages + page, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(p == MAP_FAILED && errno == EINVAL, "oversized mmap -> EINVAL (%s)",
	      p == MAP_FAILED ? strerror(errno) : "mapped!");
	p = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page);
	CHECK(p == MAP_FAILED && errno == EINVAL, "non-zero offset mmap -> EINVAL (%s)",
	      p == MAP_FAILED ? strerror(errno) : "mapped!");

	/* 4. 默认周期下 seq 每秒加 2，stamp 递增 */
	{
		uint32_t s1 = even_seq(s);
		uint64_t st1 = s->cpu[0].stamp_us;
		uint32_t s2;

		sleep_ms(3000);
		s2 = even_seq(s);
		CHECK(s2 - s1 >= 4 && s2 - s1 <= 8, "default period: seq +%u in 3 s (expect ~6)",
		      s2 - s1);
		CHECK(s->cpu[0].stamp_us > st1, "stamp_us advances (%llu -> %llu)",
		      (unsigned long long)st1, (unsigned long long)s->cpu[0].stamp_us);
	}

	/* 5. ioctl 改为 1 毫秒。用单独打开的 fd 改周期：映射会持有 fd 对应的打开文件，
	 *    只有打开文件的最后一个引用释放（关闭且无映射）时才会恢复默认。 */
	fd2 = open(CPUSTAT_DEV_PATH, O_RDONLY | O_CLOEXEC);
	rc = set_interval(fd2, 1000);
	CHECK(rc == 0, "ioctl SET_INTERVAL(1000) (%s)", rc ? strerror(errno) : "ok");
	sleep_ms(20);
	CHECK(s->interval_us == 1000, "interval_us=%u after ioctl", s->interval_us);
	{
		uint32_t s1 = even_seq(s);
		uint32_t d;

		sleep_ms(1000);
		d = even_seq(s) - s1;
		CHECK(d >= 1800 && d <= 2200, "1 ms period: seq +%u in 1 s (expect ~2000)", d);
	}

	/* 6. 参数过小 */
	rc = set_interval(fd2, 500);
	CHECK(rc != 0 && errno == EINVAL, "ioctl SET_INTERVAL(500) -> EINVAL (%s)",
	      rc ? strerror(errno) : "accepted!");
	sleep_ms(20);
	CHECK(s->interval_us == 1000, "interval_us unchanged=%u", s->interval_us);

	/* 7. 关闭改过周期的 fd（没有映射引用它）后恢复默认 */
	{
		close(fd2);
		sleep_ms(20);
		CHECK(s->interval_us == CPUSTAT_DEFAULT_INTERVAL_US,
		      "interval restored to %u after closing fd", s->interval_us);
	}

	/* 8. 改周期的进程被 kill -9 后恢复默认 */
	{
		int pipefd[2];
		pid_t pid;
		char c;

		if (pipe(pipefd) != 0)
			return 2;
		pid = fork();
		if (pid == 0) {
			int cfd = open(CPUSTAT_DEV_PATH, O_RDONLY);

			close(pipefd[0]);
			if (cfd < 0 || set_interval(cfd, 1000) != 0)
				_exit(1);
			if (write(pipefd[1], "x", 1) != 1)
				_exit(1);
			for (;;)
				pause();
		}
		close(pipefd[1]);
		rc = (int)read(pipefd[0], &c, 1);
		sleep_ms(20);
		CHECK(rc == 1 && s->interval_us == 1000, "child set interval_us=%u", s->interval_us);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		sleep_ms(20);
		CHECK(s->interval_us == CPUSTAT_DEFAULT_INTERVAL_US,
		      "interval restored to %u after kill -9", s->interval_us);
	}

	printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASS", failures);
	return failures ? 1 : 0;
}
