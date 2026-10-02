#include "xr/xr_env.h"
#include "xr/xr_log.h"
#include "xr/xr_task.h"
#include "xr/xr_time.h"
#include "xr/xr_worker.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * B2:每请求唤醒税(fan-in M→1)。M 个 producer 线程无等待地 unpark 同一 task,
 * consumer 在 worker 上按通知合并消费;统计 unpark 返回码分布与 eventfd 写/op。
 *   bench_fanin [--producers M] [--ops N] [--wkcpu C] [--prodcpu C]
 */

typedef struct
{
	xr_task_t task;
	_Atomic uint64_t parks; /* 消费到的通知数 */
	_Atomic uint64_t ticks; /* 唤醒派发次数(wait 模式的应答计数) */
	_Atomic int ready;
} fan_t;

static int fan_step(xr_task_t *t)
{
	fan_t *c = t->user;

	for (;;)
	{
		xr_park_result_t r = xr_parker_park(&t->parker);

		if (r == XR_PARK_SUSPENDED)
		{
			atomic_store_explicit(&c->ready, 1, memory_order_release);
			return XR_TASK_PARKED;
		}
		/* 消费到一个通知(DELIVER 或积压 STORED):记 tick,继续清积压 */
		atomic_fetch_add_explicit(&c->parks, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&c->ticks, 1, memory_order_relaxed);
	}
}

typedef struct
{
	xr_parker_t *parker;
	fan_t *fan;
	int ops;
	int cpu;
	int wait; /* 1=请求-响应口径:每次 unpark 等一次消费应答(复刻 PEL D5) */
	_Atomic uint64_t deliver;
	_Atomic uint64_t stored;
	_Atomic uint64_t merged;
} fan_producer_t;

static void *fan_producer(void *arg)
{
	fan_producer_t *p = arg;

	uint64_t last = 0;

	if (xr_pin_to_cpu(p->cpu) != 0)
	{
		XR_LOGW("producer pin cpu%d failed", p->cpu);
	}
	if (p->wait != 0)
	{
		last = atomic_load_explicit(&p->fan->ticks, memory_order_acquire);
	}
	for (int i = 0; i < p->ops; i++)
	{
		xr_unpark_result_t r = xr_parker_unpark(p->parker, (uint64_t)i);

		if (r == XR_UNPARK_DELIVER)
		{
			atomic_fetch_add_explicit(&p->deliver, 1,
						  memory_order_relaxed);
		}
		else if (r == XR_UNPARK_STORED)
		{
			atomic_fetch_add_explicit(&p->stored, 1,
						  memory_order_relaxed);
		}
		else
		{
			atomic_fetch_add_explicit(&p->merged, 1,
						  memory_order_relaxed);
		}
		if (p->wait != 0)
		{
			while (atomic_load_explicit(&p->fan->ticks,
						    memory_order_acquire) == last)
			{
				xr_cpu_relax();
			}
			last = atomic_load_explicit(&p->fan->ticks,
						    memory_order_acquire);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int producers = 4;
	int ops = 200000;
	int wkcpu = 1;
	int prodcpu = 2;
	int wait_mode = 0;
	fan_t *c;
	xr_worker_t *w;
	pthread_t *ths;
	fan_producer_t *ps;
	struct timespec settle = { .tv_sec = 0, .tv_nsec = 50000000 };
	uint64_t t_start, t_end, deliver = 0, stored = 0, merged = 0;
	xr_worker_stats_t stats;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--producers") == 0 && i + 1 < argc)
		{
			producers = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)
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
		else if (strcmp(argv[i], "--wait") == 0)
		{
			wait_mode = 1;
		}
	}

	xr_time_init();
	c = calloc(1, sizeof(*c));
	ths = calloc((size_t)producers, sizeof(*ths));
	ps = calloc((size_t)producers, sizeof(*ps));
	if (c == NULL || ths == NULL || ps == NULL)
	{
		return 1;
	}

	w = xr_worker_create(wkcpu);
	if (w == NULL)
	{
		XR_LOGE("worker create failed");
		return 1;
	}
	xr_task_init(&c->task, fan_step, c);
	xr_task_spawn(w, &c->task);
	while (atomic_load_explicit(&c->ready, memory_order_acquire) == 0)
	{
		xr_cpu_relax();
	}

	for (int k = 0; k < producers; k++)
	{
		ps[k].parker = &c->task.parker;
		ps[k].fan = c;
		ps[k].ops = ops;
		ps[k].cpu = prodcpu + k;
		ps[k].wait = wait_mode;
	}

	t_start = xr_now_ns();
	for (int k = 0; k < producers; k++)
	{
		if (pthread_create(&ths[k], NULL, fan_producer, &ps[k]) != 0)
		{
			XR_LOGE("producer create failed");
			return 1;
		}
	}
	for (int k = 0; k < producers; k++)
	{
		pthread_join(ths[k], NULL);
	}
	t_end = xr_now_ns();

	nanosleep(&settle, NULL);

	for (int k = 0; k < producers; k++)
	{
		deliver += atomic_load(&ps[k].deliver);
		stored += atomic_load(&ps[k].stored);
		merged += atomic_load(&ps[k].merged);
	}
	xr_worker_stats(w, &stats);

	{
		uint64_t total = (uint64_t)producers * (uint64_t)ops;
		double elapsed_s = (double)(t_end - t_start) / 1e9;
		uint64_t parks = atomic_load(&c->parks);

		printf("bench_fanin: producers=%d ops/thread=%d total=%" PRIu64
		       " wkcpu=%d prodcpu=%d..%d mode=%s\n",
		       producers, ops, total, wkcpu, prodcpu, prodcpu + producers - 1,
		       wait_mode != 0 ? "wait" : "unpaced");
		printf("  throughput: %.2f Mops/s (elapsed=%.1f ms)\n",
		       (double)total / elapsed_s / 1e6, elapsed_s * 1e3);
		printf("  unpark: deliver=%" PRIu64 " (%.1f%%) stored=%" PRIu64
		       " (%.1f%%) merged=%" PRIu64 " (%.1f%%)\n",
		       deliver, 100.0 * (double)deliver / (double)total, stored,
		       100.0 * (double)stored / (double)total, merged,
		       100.0 * (double)merged / (double)total);
		printf("  worker: parks=%" PRIu64 " runs=%" PRIu64
		       " delivers=%" PRIu64 " wake_writes=%" PRIu64 "\n",
		       parks, stats.runs, stats.delivers, stats.wake_writes);
		printf("  tax: eventfd/unpark=%.5f runs/unpark=%.5f parks/deliver=%.3f\n",
		       (double)stats.wake_writes / (double)total,
		       (double)stats.runs / (double)total,
		       deliver > 0 ? (double)parks / (double)deliver : 0.0);
	}

	xr_worker_destroy(w);
	free(ps);
	free(ths);
	free(c);
	return 0;
}
