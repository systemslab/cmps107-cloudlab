#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sched.h>
#include <sys/syscall.h>

int
main(int argc, char **argv)
{
	int32_t cpu;

	cpu = sched_getcpu();

	printf("Current CPU number: %d\n", cpu);
	exit(EXIT_SUCCESS);
}
