#define _GNU_SOURCE
#include "xr/xr_env.h"

#include "xr/xr_log.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

int xr_cpu_count(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int)n : 1;
}

int xr_pin_to_cpu(int cpu)
{
	cpu_set_t set;

	if (cpu < 0 || cpu >= CPU_SETSIZE)
	{
		return -EINVAL;
	}
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
	{
		return -errno;
	}
	return 0;
}

int xr_current_cpu(void)
{
	int c = sched_getcpu();
	return c >= 0 ? c : -1;
}

void xr_pin_or_die(int cpu)
{
	int rc = xr_pin_to_cpu(cpu);
	if (rc != 0)
	{
		XR_LOGE("pin cpu%d failed: %d", cpu, rc);
		exit(1);
	}
}
