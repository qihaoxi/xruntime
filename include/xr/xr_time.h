#ifndef XR_TIME_H
#define XR_TIME_H

#include <stdint.h>

/*
 * 时间源(基准测量用):
 * - xr_now_ns():CLOCK_MONOTONIC(vDSO,~20ns/次),用于跨段/跨线程时间戳;
 * - xr_tsc():周期级段内计时(绑核后稳定),xr_tsc_to_ns() 换算;
 * 先调 xr_time_init() 校准 TSC 频率(20ms 窗口)。
 */
void xr_time_init(void);

uint64_t xr_now_ns(void);
uint64_t xr_tsc(void);
uint64_t xr_tsc_hz(void);

static inline uint64_t xr_tsc_to_ns(uint64_t tsc)
{
	uint64_t hz = xr_tsc_hz();
	if (hz == 0)
	{
		return 0;
	}
	return (tsc * 1000000000ull) / hz;
}

static inline void xr_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

#endif
