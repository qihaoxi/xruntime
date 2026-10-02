#include "xr/xr_task.h"
#include "xr/xr_time.h"
#include "xr/xr_worker.h"

#include <stdatomic.h>
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

typedef struct
{
	xr_task_t task;
	_Atomic int ready;
	_Atomic int runs;
} wk_t;

static int wk_step(xr_task_t *t)
{
	wk_t *c = t->user;
	xr_park_result_t r = xr_parker_park(&t->parker);

	if (r == XR_PARK_SUSPENDED)
	{
		/* 首次进入(或极少数重排路径):已挂起并宣告就绪 */
		atomic_store_explicit(&c->ready, 1, memory_order_release);
		return XR_TASK_PARKED;
	}

	/* CONSUMED:唤醒后的派发,消费通知并重新挂起 */
	{
		xr_park_result_t r2 = xr_parker_park(&t->parker);

		if (r2 != XR_PARK_SUSPENDED)
		{
			return XR_TASK_RUN_AGAIN;
		}
		atomic_fetch_add_explicit(&c->runs, 1, memory_order_relaxed);
		atomic_store_explicit(&c->ready, 1, memory_order_release);
		return XR_TASK_PARKED;
	}
}

int main(void)
{
	const int n = 10000;
	xr_worker_t *w = xr_worker_create(-1);
	wk_t c = { 0 };
	xr_worker_stats_t stats;

	CHECK(w != NULL);
	if (w == NULL)
	{
		return 1;
	}

	xr_task_init(&c.task, wk_step, &c);
	xr_task_spawn(w, &c.task);

	while (atomic_load_explicit(&c.ready, memory_order_acquire) == 0)
	{
		xr_cpu_relax();
	}

	for (int i = 0; i < n; i++)
	{
		xr_unpark_result_t ur =
			xr_parker_unpark(&c.task.parker, (uint64_t)i);

		CHECK(ur == XR_UNPARK_DELIVER);
		while (atomic_load_explicit(&c.runs, memory_order_acquire) <
		       i + 1)
		{
			xr_cpu_relax();
		}
	}

	CHECK(atomic_load(&c.runs) == n);

	xr_worker_stats(w, &stats);
	CHECK(stats.delivers == (uint64_t)n);
	CHECK(stats.wake_writes > 0);
	CHECK(stats.wake_writes <= (uint64_t)n);
	CHECK(stats.runs >= (uint64_t)n);

	xr_worker_destroy(w);

	if (failures != 0)
	{
		fprintf(stderr, "worker: %d failure(s)\n", failures);
		return 1;
	}
	printf("worker: OK (roundtrips=%d delivers=%llu wake_writes=%llu)\n", n,
	       (unsigned long long)stats.delivers,
	       (unsigned long long)stats.wake_writes);
	return 0;
}
