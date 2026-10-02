#include "xr/xr_log.h"

#include "xr/xr_time.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>

static _Atomic int g_level = XR_LOG_INFO;

void xr_log_set_level(xr_log_level_t lvl)
{
	atomic_store_explicit(&g_level, (int)lvl, memory_order_relaxed);
}

xr_log_level_t xr_log_get_level(void)
{
	return (xr_log_level_t)atomic_load_explicit(&g_level, memory_order_relaxed);
}

void xr_logf(xr_log_level_t lvl, const char *file, int line, const char *fmt, ...)
{
	static const char *const names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
	va_list ap;
	uint64_t ns;
	unsigned long sec, usec;

	if ((int)lvl < atomic_load_explicit(&g_level, memory_order_relaxed))
	{
		return;
	}

	ns = xr_now_ns();
	sec = (unsigned long)(ns / 1000000000ull);
	usec = (unsigned long)((ns % 1000000000ull) / 1000ull);
	fprintf(stderr, "[%lu.%06lu] %-5s %s:%d: ", sec, usec,
		names[(int)lvl], file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
