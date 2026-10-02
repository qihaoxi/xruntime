#include "xr/xr_env.h"
#include "xr/xr_log.h"
#include "xr/xr_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 测量环境体检 + 时间源自身成本(基准前先跑,确认绑核与 TSC 可用)。
 *   bench_env [--cpu N] [--iters N]
 */

static uint64_t bench_now_ns(int iters)
{
	uint64_t t0 = xr_now_ns();
	uint64_t sum = 0;

	for (int i = 0; i < iters; i++)
	{
		sum += xr_now_ns();
	}
	uint64_t t1 = xr_now_ns();
	if (sum == 0)
	{
		return 0; /* 防优化消除 */
	}
	return (t1 - t0) / (uint64_t)iters;
}

static uint64_t bench_tsc_read_ns(int iters)
{
	uint64_t t0 = xr_tsc();
	uint64_t sum = 0;

	for (int i = 0; i < iters; i++)
	{
		sum += xr_tsc();
	}
	uint64_t t1 = xr_tsc();
	if (sum == 0)
	{
		return 0;
	}
	return xr_tsc_to_ns(t1 - t0) / (uint64_t)iters;
}

int main(int argc, char **argv)
{
	int cpu = -1;
	int iters = 100000;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc)
		{
			cpu = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
		{
			iters = atoi(argv[++i]);
		}
	}

	xr_time_init();
	if (cpu >= 0)
	{
		int rc = xr_pin_to_cpu(cpu);
		if (rc != 0)
		{
			XR_LOGE("pin cpu%d failed: %d", cpu, rc);
			return 1;
		}
	}

	printf("cpu_count=%d current_cpu=%d tsc_hz=%" PRIu64 "\n",
	       xr_cpu_count(), xr_current_cpu(), xr_tsc_hz());
	printf("self_cost iters=%d: now_ns=%" PRIu64 " ns/op, tsc_read=%" PRIu64
	       " ns/op\n",
	       iters, bench_now_ns(iters), bench_tsc_read_ns(iters));
	return 0;
}
