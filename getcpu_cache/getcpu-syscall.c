#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/syscall.h>

int
main(int argc, char **argv)
{
	int32_t cpu, status;
	status = syscall(SYS_getcpu, &cpu, NULL, NULL);

	printf("Current CPU number: %d\n", cpu);
	exit(EXIT_SUCCESS);
}
