#include "xr/xr_env.h"
#include "xr/xr_log.h"
#include "xr/xr_time.h"

#include <inttypes.h>
#include <stdio.h>

static int failures;

#define CHECK(cond)                                                            \
	do                                                                     \
	{                                                                      \
		if (!(cond))                                                   \
		{                                                              \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				#cond);                                        \
			failures++;                                            \
		}                                                              \
	} while (0)

int main(void)
{
	uint64_t a, b, sum = 0;
	int pin;

	xr_time_init();
	CHECK(xr_cpu_count() >= 1);
	CHECK(xr_tsc_hz() > 0);

	a = xr_now_ns();
	for (int i = 0; i < 1000; i++)
	{
		sum += xr_now_ns();
	}
	b = xr_now_ns();
	CHECK(b > a);
	CHECK(sum >= a);

	xr_log_set_level(XR_LOG_INFO);
	CHECK(xr_log_get_level() == XR_LOG_INFO);
	XR_LOGI("tsc_hz=%" PRIu64 " cpu_count=%d current_cpu=%d",
		xr_tsc_hz(), xr_cpu_count(), xr_current_cpu());

	pin = xr_pin_to_cpu(0);
	if (pin == 0)
	{
		CHECK(xr_current_cpu() == 0);
	}
	else
	{
		XR_LOGW("pin cpu0 unavailable: %d (container?)", pin);
	}

	if (failures != 0)
	{
		fprintf(stderr, "smoke: %d failure(s)\n", failures);
		return 1;
	}
	printf("smoke: OK\n");
	return 0;
}
