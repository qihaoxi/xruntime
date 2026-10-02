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

/* ---------- 跨线程往返:外部线程 unpark,worker 上 task 恢复 ---------- */

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
		atomic_store_explicit(&c->ready, 1, memory_order_release);
		return XR_TASK_PARKED;
	}

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

static void run_cross_thread(unsigned flags)
{
	const int n = 10000;
	xr_worker_t *w = xr_worker_create(-1);
	wk_t c = { 0 };
	xr_worker_stats_t stats;

	CHECK(w != NULL);
	if (w == NULL)
	{
		return;
	}
	xr_worker_set_flags(w, flags);

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
}

/* ---------- 同线程 ping-pong:两个 task 在同一 worker 上互踢 ---------- */

typedef struct
{
	xr_task_t task;
	xr_parker_t *peer;
	_Atomic uint64_t *total; /* 共享 hop 计数:到 n 时该棒 DONE */
	_Atomic int ready;
	int n;
} pp_t;

static int pp_step(xr_task_t *t)
{
	pp_t *p = t->user;
	xr_park_result_t r = xr_parker_park(&t->parker);

	if (r == XR_PARK_SUSPENDED)
	{
		atomic_store_explicit(&p->ready, 1, memory_order_release);
		return XR_TASK_PARKED;
	}
	{
		uint64_t i = atomic_fetch_add_explicit(p->total, 1,
						       memory_order_relaxed);

		if (i + 1 >= (uint64_t)p->n)
		{
			return XR_TASK_DONE;
		}
		xr_parker_unpark(p->peer, i);
		r = xr_parker_park(&t->parker);
		if (r != XR_PARK_SUSPENDED)
		{
			return XR_TASK_RUN_AGAIN;
		}
		return XR_TASK_PARKED;
	}
}

static void run_same_thread(unsigned flags)
{
	const int n = 100000;
	xr_worker_t *w = xr_worker_create(-1);
	pp_t a = { 0 };
	pp_t b = { 0 };
	_Atomic uint64_t total = 0;
	xr_worker_stats_t stats;

	CHECK(w != NULL);
	if (w == NULL)
	{
		return;
	}
	xr_worker_set_flags(w, flags);

	a.peer = &b.task.parker;
	a.total = &total;
	a.n = n;
	b.peer = &a.task.parker;
	b.total = &total;
	b.n = n;
	xr_task_init(&a.task, pp_step, &a);
	xr_task_init(&b.task, pp_step, &b);
	xr_task_spawn(w, &a.task);
	xr_task_spawn(w, &b.task);

	while (atomic_load_explicit(&a.ready, memory_order_acquire) == 0 ||
	       atomic_load_explicit(&b.ready, memory_order_acquire) == 0)
	{
		xr_cpu_relax();
	}

	xr_parker_unpark(&a.task.parker, 0);

	while (atomic_load_explicit(&total, memory_order_acquire) <
	       (uint64_t)n)
	{
		xr_cpu_relax();
	}
	CHECK(atomic_load(&total) == (uint64_t)n);

	xr_worker_stats(w, &stats);
	CHECK(stats.runs >= (uint64_t)n);
	if ((flags & XR_WORKER_DIRECT) != 0)
	{
		CHECK(stats.wake_direct > 0);
	}

	xr_worker_destroy(w);
}

int main(void)
{
	const unsigned variants[] = {
		0,
		XR_WORKER_DIRECT,
		XR_WORKER_GATE,
		XR_WORKER_DIRECT | XR_WORKER_GATE,
	};

	for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++)
	{
		run_cross_thread(variants[v]);
		run_same_thread(variants[v]);
	}

	if (failures != 0)
	{
		fprintf(stderr, "worker: %d failure(s)\n", failures);
		return 1;
	}
	printf("worker: OK (4 flags x cross-thread+same-thread)\n");
	return 0;
}
