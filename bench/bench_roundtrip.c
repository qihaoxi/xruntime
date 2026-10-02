#include "xr/xr_env.h"
#include "xr/xr_log.h"
#include "xr/xr_task.h"
#include "xr/xr_time.h"
#include "xr/xr_worker.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * B1:唤醒延迟分解。
 *  cross-thread(默认):producer(prodcpu) unpark → worker(wkcpu) 上 task 恢复。
 *  same-thread(--same-thread):producer/consumer 两个 task 同 worker 互踢。
 * 段:t0=unpark 进入,t1=unpark 返回(claim/publish/deliver/registry/队列/
 * transport 全含),t2=消费侧恢复(传输+loop tick+调度+恢复)。
 *  L0 对照(--l0=fibre):同一机制(flags 不变)换成 stackful 阻塞式 body;
 *  stackless 的 t2 在 step 入口,fibre 的 t2 在 xr_task_wait 恢复后(含消费)。
 *   bench_roundtrip [--ops N] [--wkcpu C] [--prodcpu C]
 *                   [--same-thread] [--flags N] [--l0=stackless|fibre]
 */

typedef struct
{
	uint64_t t0;
	uint64_t t1;
	uint64_t t2;
	uint8_t res;
} rt_sample_t;

typedef struct
{
	xr_task_t task;
	rt_sample_t *samples;
	xr_parker_t *next; /* same-thread:唤醒后踢下一棒 */
	_Atomic uint64_t cur;
	_Atomic uint64_t resume_idx;
	_Atomic int ready;
	_Atomic int stop; /* fibre:终止信号 */
	_Atomic int done; /* fibre:entry 即将返回 */
} rt_t;

static int rt_step(xr_task_t *t)
{
	rt_t *c = t->user;
	uint64_t t2 = xr_tsc();
	xr_park_result_t r = xr_parker_park(&t->parker);

	if (r == XR_PARK_SUSPENDED)
	{
		/* 首次运行:挂起并宣告就绪 */
		atomic_store_explicit(&c->ready, 1, memory_order_release);
		return XR_TASK_PARKED;
	}

	/* CONSUMED:一次唤醒的派发;消费通知后重新挂起 */
	{
		xr_park_result_t r2 = xr_parker_park(&t->parker);

		if (r2 != XR_PARK_SUSPENDED)
		{
			return XR_TASK_RUN_AGAIN;
		}
		if (atomic_load_explicit(&c->ready, memory_order_acquire) == 0)
		{
			atomic_store_explicit(&c->ready, 1, memory_order_release);
			return XR_TASK_PARKED;
		}
		{
			uint64_t i = atomic_load_explicit(&c->cur,
							  memory_order_acquire);

			c->samples[i].t2 = t2;
			atomic_store_explicit(&c->resume_idx, i + 1,
					      memory_order_release);
			if (c->next != NULL)
			{
				xr_parker_unpark(c->next, i);
			}
		}
		return XR_TASK_PARKED;
	}
}

/* same-thread:producer 棒,唤醒时 unpark consumer 后自挂 */
typedef struct
{
	xr_task_t task;
	rt_t *cons;
	int ops;
	_Atomic uint64_t idx;
	_Atomic int ready;
	_Atomic int prod_done; /* fibre:entry 即将返回 */
	uint64_t res_count[3];
} rt_prod_t;

static int rt_prod_step(xr_task_t *t)
{
	rt_prod_t *p = t->user;
	xr_park_result_t r = xr_parker_park(&t->parker);

	if (r == XR_PARK_SUSPENDED)
	{
		atomic_store_explicit(&p->ready, 1, memory_order_release);
		return XR_TASK_PARKED;
	}
	{
		uint64_t i = atomic_load_explicit(&p->idx, memory_order_relaxed);

		if (i >= (uint64_t)p->ops)
		{
			return XR_TASK_DONE;
		}
		atomic_store_explicit(&p->cons->cur, i, memory_order_release);
		{
			uint64_t t0 = xr_tsc();
			xr_unpark_result_t ur =
				xr_parker_unpark(&p->cons->task.parker, i);
			uint64_t t1 = xr_tsc();

			p->cons->samples[i].t0 = t0;
			p->cons->samples[i].t1 = t1;
			p->cons->samples[i].res = (uint8_t)ur;
			p->res_count[ur]++;
		}
		atomic_store_explicit(&p->idx, i + 1, memory_order_release);
		r = xr_parker_park(&t->parker);
		if (r != XR_PARK_SUSPENDED)
		{
			return XR_TASK_RUN_AGAIN;
		}
		return XR_TASK_PARKED;
	}
}

/* ---------- L0 stackful body(阻塞式,机制/测量点与 stackless 相同) ---------- */

static void rtf_cons_entry(xr_task_t *t)
{
	rt_t *c = t->user;

	atomic_store_explicit(&c->ready, 1, memory_order_release);
	for (;;)
	{
		xr_task_wait(t);
		if (atomic_load_explicit(&c->stop, memory_order_acquire) != 0)
		{
			break;
		}
		{
			uint64_t i = atomic_load_explicit(&c->cur,
							  memory_order_acquire);
			uint64_t t2 = xr_tsc();

			c->samples[i].t2 = t2;
			atomic_store_explicit(&c->resume_idx, i + 1,
					      memory_order_release);
			if (c->next != NULL)
			{
				xr_parker_unpark(c->next, i);
			}
		}
	}
	atomic_store_explicit(&c->done, 1, memory_order_release);
}

static void rtf_prod_entry(xr_task_t *t)
{
	rt_prod_t *p = t->user;
	rt_t *c = p->cons;

	atomic_store_explicit(&p->ready, 1, memory_order_release);
	xr_task_wait(t); /* 主线程 kick */
	for (uint64_t i = 0; i < (uint64_t)p->ops; i++)
	{
		uint64_t t0, t1;
		xr_unpark_result_t ur;

		atomic_store_explicit(&c->cur, i, memory_order_release);
		t0 = xr_tsc();
		ur = xr_parker_unpark(&c->task.parker, i);
		t1 = xr_tsc();
		c->samples[i].t0 = t0;
		c->samples[i].t1 = t1;
		c->samples[i].res = (uint8_t)ur;
		p->res_count[ur]++;
		xr_task_wait(t);
	}
	atomic_store_explicit(&c->stop, 1, memory_order_release);
	xr_parker_unpark(&c->task.parker, 0);
	atomic_store_explicit(&p->prod_done, 1, memory_order_release);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	if (x < y)
	{
		return -1;
	}
	if (x > y)
	{
		return 1;
	}
	return 0;
}

static void report(const char *name, uint64_t *v, size_t n)
{
	uint64_t sum = 0;

	qsort(v, n, sizeof(uint64_t), cmp_u64);
	for (size_t i = 0; i < n; i++)
	{
		sum += v[i];
	}
	printf("  %-13s p50=%7" PRIu64 " ns  p90=%8" PRIu64 "  p99=%9" PRIu64
	       "  max=%10" PRIu64 "  avg=%8" PRIu64 "\n",
	       name, xr_tsc_to_ns(v[n / 2]), xr_tsc_to_ns(v[n * 9 / 10]),
	       xr_tsc_to_ns(v[n * 99 / 100]), xr_tsc_to_ns(v[n - 1]),
	       xr_tsc_to_ns(sum / n));
}

int main(int argc, char **argv)
{
	int ops = 200000;
	int wkcpu = 1;
	int prodcpu = 2;
	int neg = 0;
	int same_thread = 0;
	int l0_fibre = 0;
	unsigned flags = 0;
	rt_t *c;
	xr_worker_t *w;
	xr_worker_stats_t wstats;
	uint64_t res_count[3] = { 0, 0, 0 };
	uint64_t *prod, *cons, *rtt;
	uint64_t t_start, t_end;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)
		{
			ops = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--wkcpu") == 0 && i + 1 < argc)
		{
			wkcpu = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--prodcpu") == 0 && i + 1 < argc)
		{
			prodcpu = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--flags") == 0 && i + 1 < argc)
		{
			flags = (unsigned)strtoul(argv[++i], NULL, 0);
		}
		else if (strcmp(argv[i], "--same-thread") == 0)
		{
			same_thread = 1;
		}
		else if (strcmp(argv[i], "--l0") == 0 && i + 1 < argc)
		{
			l0_fibre = strcmp(argv[++i], "fibre") == 0;
		}
	}

	xr_time_init();
	c = calloc(1, sizeof(*c));
	if (c == NULL)
	{
		return 1;
	}
	c->samples = calloc((size_t)ops, sizeof(rt_sample_t));
	prod = malloc((size_t)ops * sizeof(uint64_t));
	cons = malloc((size_t)ops * sizeof(uint64_t));
	rtt = malloc((size_t)ops * sizeof(uint64_t));
	if (c->samples == NULL || prod == NULL || cons == NULL || rtt == NULL)
	{
		return 1;
	}

	w = xr_worker_create(wkcpu);
	if (w == NULL)
	{
		XR_LOGE("worker create failed");
		return 1;
	}
	xr_worker_set_flags(w, flags);

	if (same_thread != 0)
	{
		rt_prod_t *p = calloc(1, sizeof(*p));

		if (p == NULL)
		{
			return 1;
		}
		p->cons = c;
		p->ops = ops;
		c->next = &p->task.parker;
		if (l0_fibre != 0)
		{
			if (xr_task_init_fibre(&c->task, rtf_cons_entry, 0) != 0 ||
			    xr_task_init_fibre(&p->task, rtf_prod_entry, 0) != 0)
			{
				XR_LOGE("fibre init failed");
				return 1;
			}
			c->task.user = c;
			p->task.user = p;
		}
		else
		{
			xr_task_init(&c->task, rt_step, c);
			xr_task_init(&p->task, rt_prod_step, p);
		}
		xr_task_spawn(w, &p->task);
		xr_task_spawn(w, &c->task);

		while (atomic_load_explicit(&c->ready, memory_order_acquire) == 0 ||
		       atomic_load_explicit(&p->ready, memory_order_acquire) == 0)
		{
			xr_cpu_relax();
		}
		t_start = xr_now_ns();
		xr_parker_unpark(&p->task.parker, 0);
		if (l0_fibre != 0)
		{
			while (atomic_load_explicit(&p->prod_done,
						    memory_order_acquire) == 0 ||
			       atomic_load_explicit(&c->done,
						    memory_order_acquire) == 0)
			{
				xr_cpu_relax();
			}
		}
		else
		{
			while (atomic_load_explicit(&p->idx,
						    memory_order_acquire) <
			       (uint64_t)ops)
			{
				xr_cpu_relax();
			}
			while (atomic_load_explicit(&c->resume_idx,
						    memory_order_acquire) <
			       (uint64_t)ops)
			{
				xr_cpu_relax();
			}
		}
		t_end = xr_now_ns();
		for (int i = 0; i < 3; i++)
		{
			res_count[i] = p->res_count[i];
		}
		free(p);
	}
	else
	{
		if (l0_fibre != 0)
		{
			if (xr_task_init_fibre(&c->task, rtf_cons_entry, 0) != 0)
			{
				XR_LOGE("fibre init failed");
				return 1;
			}
			c->task.user = c;
		}
		else
		{
			xr_task_init(&c->task, rt_step, c);
		}
		xr_task_spawn(w, &c->task);

		if (xr_pin_to_cpu(prodcpu) != 0)
		{
			XR_LOGE("pin producer cpu%d failed", prodcpu);
			xr_worker_destroy(w);
			return 1;
		}
		while (atomic_load_explicit(&c->ready, memory_order_acquire) == 0)
		{
			xr_cpu_relax();
		}

		t_start = xr_now_ns();
		for (int i = 0; i < ops; i++)
		{
			uint64_t t0, t1;
			xr_unpark_result_t ur;

			atomic_store_explicit(&c->cur, (uint64_t)i,
					      memory_order_release);
			t0 = xr_tsc();
			ur = xr_parker_unpark(&c->task.parker, (uint64_t)i);
			t1 = xr_tsc();
			c->samples[i].t0 = t0;
			c->samples[i].t1 = t1;
			c->samples[i].res = (uint8_t)ur;
			res_count[ur]++;
			while (atomic_load_explicit(&c->resume_idx,
						    memory_order_acquire) <=
			       (uint64_t)i)
			{
				xr_cpu_relax();
			}
		}
		if (l0_fibre != 0)
		{
			atomic_store_explicit(&c->stop, 1, memory_order_release);
			xr_parker_unpark(&c->task.parker, 0);
			while (atomic_load_explicit(&c->done,
						    memory_order_acquire) == 0)
			{
				xr_cpu_relax();
			}
		}
		t_end = xr_now_ns();
	}

	for (int i = 0; i < ops; i++)
	{
		int64_t d2 = (int64_t)(c->samples[i].t2 - c->samples[i].t1);
		int64_t dr = (int64_t)(c->samples[i].t2 - c->samples[i].t0);

		if (d2 < 0 || dr < 0)
		{
			neg++;
			d2 = d2 < 0 ? 0 : d2;
			dr = dr < 0 ? 0 : dr;
		}
		prod[i] = c->samples[i].t1 - c->samples[i].t0;
		cons[i] = (uint64_t)d2;
		rtt[i] = (uint64_t)dr;
	}

	xr_worker_stats(w, &wstats);
	printf("bench_roundtrip: ops=%d wkcpu=%d prodcpu=%d mode=%s l0=%s flags=%u\n",
	       ops, wkcpu, prodcpu,
	       same_thread != 0 ? "same-thread" : "cross-thread",
	       l0_fibre != 0 ? "fibre" : "stackless", flags);
	printf("  unpark: deliver=%" PRIu64 " stored=%" PRIu64
	       " merged=%" PRIu64 " neg_delta=%d\n",
	       res_count[XR_UNPARK_DELIVER], res_count[XR_UNPARK_STORED],
	       res_count[XR_UNPARK_MERGED], neg);
	report("producer_side", prod, (size_t)ops);
	report("consumer_wake", cons, (size_t)ops);
	report("rtt", rtt, (size_t)ops);
	printf("  worker: wake_writes=%" PRIu64 " wake_direct=%" PRIu64
	       " wake_gated=%" PRIu64 "\n",
	       wstats.wake_writes, wstats.wake_direct, wstats.wake_gated);
	printf("  loop: elapsed=%.1f ms avg_rtt=%.1f us\n",
	       (double)(t_end - t_start) / 1e6,
	       (double)(t_end - t_start) / 1000.0 / ops);

	xr_worker_destroy(w);
	free(rtt);
	free(cons);
	free(prod);
	free(c->samples);
	free(c);
	return 0;
}
