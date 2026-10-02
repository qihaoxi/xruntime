#include "xr/xr_time.h"

#include <stdatomic.h>
#include <time.h>

static _Atomic uint64_t g_tsc_hz;

uint64_t xr_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t xr_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
	uint32_t lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
	uint64_t v;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
	return v;
#else
	return xr_now_ns();
#endif
}

uint64_t xr_tsc_hz(void)
{
	return atomic_load_explicit(&g_tsc_hz, memory_order_relaxed);
}

void xr_time_init(void)
{
	struct timespec t0, t1;
	struct timespec req;
	uint64_t c0, c1, ns;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	c0 = xr_tsc();

	req.tv_sec = 0;
	req.tv_nsec = 20000000; /* 20ms 校准窗 */
	nanosleep(&req, NULL);

	c1 = xr_tsc();
	clock_gettime(CLOCK_MONOTONIC, &t1);

	ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
	     (uint64_t)(t1.tv_nsec - t0.tv_nsec);
	if (ns == 0 || c1 <= c0)
	{
		atomic_store_explicit(&g_tsc_hz, 0, memory_order_relaxed);
		return;
	}
	atomic_store_explicit(&g_tsc_hz, (c1 - c0) * 1000000000ull / ns,
			      memory_order_relaxed);
}
