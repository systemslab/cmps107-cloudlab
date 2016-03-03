#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sched.h>
#include <linux/getcpu_cache.h>
#include <sys/syscall.h>

static inline int
getcpu_cache(int cmd, volatile int32_t **cpu_cachep, int flags)
{
	return syscall(__NR_getcpu_cache, cmd, cpu_cachep, flags);
}
/*
 * __getcpu_cache_tls is recommended as symbol name for the
 * cpu number cache. Weak attribute is recommended when
 * declaring this variable in libraries. Applications can
 * choose to define their own version of this symbol without
 * the weak attribute and access it directly as a
 * performance improvement when it matches the address
 * returned by GETCPU_CACHE_GET. The initial value "-1"
 * will be read in case the getcpu cache is not available.
 */
__thread __attribute__((weak)) volatile int32_t
	__getcpu_cache_tls = -1;
int
main(int argc, char **argv)
{
	volatile int32_t *cpu_cache = &__getcpu_cache_tls;
	int32_t cpu;
	/* Try to register the CPU cache. */
	if (getcpu_cache(GETCPU_CACHE_SET, &cpu_cache, 0) < 0) {
		perror("getcpu_cache set");
		fprintf(stderr, "Using sched_getcpu() as fallback.\n");
	}
	cpu = __getcpu_cache_tls;	/* Read current CPU number. */
	if (cpu < 0) {
		/* Fallback on sched_getcpu(). */
		cpu = sched_getcpu();
	}
	printf("Current CPU number: %d\n", cpu);
	exit(EXIT_SUCCESS);
}
