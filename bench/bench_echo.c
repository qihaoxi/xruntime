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

/*
 * B3:高并发合成 echo(K 个 conn task + M 个 driver)。每请求:
 *   driver 投递 req → unpark conn → conn 处理 → ack driver。
 * ack 两种口径:
 *   --ack=spin:driver 为 pthread,自旋等 ack 计数(上限估计,1 对 park/wake);
 *   --ack=park:driver 为第二个 worker 上的 task,ack 即 unpark driver
 *               (忠实 PEL 阻塞 echo 的 2 对 park/wake)。
 *   --l0=fibre:conn/driver 用 stackful 阻塞 body(机制/flags/测量点不变),
 *              与 stackless 对照"每请求 park 次数"成本。
 * 指标:rps、req→ack p50/p99、transport/req、runs/req、合并率。
 *   bench_echo [--conns K] [--drivers M] [--ops N] [--wkcpu C] [--drcpu C]
 *              [--flags N] [--ack=spin|park] [--l0=stackless|fibre]
 */

#define EB_QUEUE_CAP 256
#define EB_MAX_DRAIN 64

typedef struct eb eb_t;

typedef struct
{
	xr_task_t task;
	eb_t *b;
	int id;
	_Atomic int ready;
	pthread_mutex_t lock;
	int req_drv[EB_QUEUE_CAP];
	int head;
	int tail;
	int count;
} eb_conn_t;

typedef struct
{
	xr_task_t task;
	eb_t *b;
	int id;
	int cpu;
	int ops;
	uint64_t *lat; /* ops 个 TSC 样本 */
	_Atomic uint64_t ack; /* spin 口径 */
	uint64_t t0;
	int done;
	int inflight; /* stackless 首发守卫 */
} eb_driver_t;

struct eb
{
	xr_worker_t *wc; /* conn worker */
	xr_worker_t *wd; /* driver worker(park 口径) */
	int k;
	int m;
	int ops;
	int park_mode;
	unsigned flags;
	eb_conn_t *conns;
	eb_driver_t *drivers;
	_Atomic int conns_ready;
	_Atomic int drivers_done;
	_Atomic int conns_done;
	_Atomic int stop; /* fibre:L0 收尾信号 */
	_Atomic uint64_t acked;
};

/* ---------- conn ---------- */

static int conn_step(xr_task_t *t)
{
	eb_conn_t *c = t->user;
	eb_t *b = c->b;

	for (;;)
	{
		xr_park_result_t r = xr_parker_park(&t->parker);

		if (r == XR_PARK_SUSPENDED)
		{
			if (atomic_exchange_explicit(&c->ready, 1,
						     memory_order_acq_rel) == 0)
			{
				atomic_fetch_add_explicit(&b->conns_ready, 1,
							  memory_order_release);
			}
			return XR_TASK_PARKED;
		}

		/* 消费一个通知:锁内摘取一批,锁外 ack(延迟唤醒) */
		for (;;)
		{
			int batch[EB_MAX_DRAIN];
			int n = 0;

			pthread_mutex_lock(&c->lock);
			while (c->count > 0 && n < EB_MAX_DRAIN)
			{
				batch[n++] = c->req_drv[c->head];
				c->head = (c->head + 1) % EB_QUEUE_CAP;
				c->count--;
			}
			pthread_mutex_unlock(&c->lock);

			for (int i = 0; i < n; i++)
			{
				eb_driver_t *d = &b->drivers[batch[i]];

				if (b->park_mode != 0)
				{
					xr_parker_unpark(&d->task.parker, 0);
				}
				else
				{
					atomic_fetch_add_explicit(&d->ack, 1,
							memory_order_release);
				}
			}
			atomic_fetch_add_explicit(&b->acked, (uint64_t)n,
						  memory_order_relaxed);
			if (n < EB_MAX_DRAIN)
			{
				break;
			}
		}
	}
}

static void conn_post(eb_conn_t *c, int drv)
{
	pthread_mutex_lock(&c->lock);
	if (c->count >= EB_QUEUE_CAP)
	{
		/* 单 driver 至多 1 个 in-flight,容量=256 足够;防御丢弃 */
		pthread_mutex_unlock(&c->lock);
		return;
	}
	c->req_drv[c->tail] = drv;
	c->tail = (c->tail + 1) % EB_QUEUE_CAP;
	c->count++;
	pthread_mutex_unlock(&c->lock);
}

/* ---------- driver:spin 口径(pthread) ---------- */

static void *drv_thread(void *arg)
{
	eb_driver_t *d = arg;
	eb_t *b = d->b;

	if (d->cpu >= 0 && xr_pin_to_cpu(d->cpu) != 0)
	{
		XR_LOGW("driver pin cpu%d failed", d->cpu);
	}
	for (int i = 0; i < d->ops; i++)
	{
		eb_conn_t *c = &b->conns[(d->id + i) % b->k];
		uint64_t t0 = xr_tsc();

		conn_post(c, d->id);
		xr_parker_unpark(&c->task.parker, 0);
		while (atomic_load_explicit(&d->ack, memory_order_acquire) <
		       (uint64_t)i + 1)
		{
			xr_cpu_relax();
		}
		d->lat[i] = xr_tsc() - t0;
	}
	return NULL;
}

/* ---------- driver:park 口径(task,driver worker 上) ----------
 * stackless 续体每次唤醒都从函数头重入:首发必须用 inflight 守卫,且
 * deliver 的 NOTIFIED 只能由循环顶的 park 消费。旧两版分别犯"幻影 op"
 * (park 在尾、吞掉本次 deliver)与"重发重置 t0"(首发块每次重入都执行)
 * 的错误,fibre 对照轮暴露;现形态与 conn_step/drv_thread 语义一致。 */
static int drv_step(xr_task_t *t)
{
	eb_driver_t *d = t->user;
	eb_t *b = d->b;

	if (d->inflight == 0)
	{
		eb_conn_t *c = &b->conns[d->id % b->k];

		d->t0 = xr_tsc();
		conn_post(c, d->id);
		xr_parker_unpark(&c->task.parker, 0);
		d->inflight = 1;
	}
	for (;;)
	{
		xr_park_result_t r = xr_parker_park(&t->parker);

		if (r == XR_PARK_SUSPENDED)
		{
			return XR_TASK_PARKED;
		}
		/* CONSUMED:在途请求的 ack 到达 */
		d->lat[d->done] = xr_tsc() - d->t0;
		d->done++;
		if (d->done >= d->ops)
		{
			atomic_fetch_add_explicit(&b->drivers_done, 1,
						  memory_order_release);
			return XR_TASK_DONE;
		}
		{
			eb_conn_t *c =
				&b->conns[(d->id + d->done) % b->k];

			d->t0 = xr_tsc();
			conn_post(c, d->id);
			xr_parker_unpark(&c->task.parker, 0);
			d->inflight = 1;
		}
	}
}

/* ---------- L0 fibre body(阻塞式;机制/测量点与 stackless 相同) ---------- */

static void conn_fibre_entry(xr_task_t *t)
{
	eb_conn_t *c = t->user;
	eb_t *b = c->b;

	atomic_fetch_add_explicit(&b->conns_ready, 1, memory_order_release);
	for (;;)
	{
		xr_task_wait(t);
		if (atomic_load_explicit(&b->stop, memory_order_acquire) != 0)
		{
			break;
		}
		for (;;)
		{
			int batch[EB_MAX_DRAIN];
			int n = 0;

			pthread_mutex_lock(&c->lock);
			while (c->count > 0 && n < EB_MAX_DRAIN)
			{
				batch[n++] = c->req_drv[c->head];
				c->head = (c->head + 1) % EB_QUEUE_CAP;
				c->count--;
			}
			pthread_mutex_unlock(&c->lock);

			for (int i = 0; i < n; i++)
			{
				eb_driver_t *d = &b->drivers[batch[i]];

				if (b->park_mode != 0)
				{
					xr_parker_unpark(&d->task.parker, 0);
				}
				else
				{
					atomic_fetch_add_explicit(&d->ack, 1,
							memory_order_release);
				}
			}
			atomic_fetch_add_explicit(&b->acked, (uint64_t)n,
						  memory_order_relaxed);
			if (n < EB_MAX_DRAIN)
			{
				break;
			}
		}
	}
	atomic_fetch_add_explicit(&b->conns_done, 1, memory_order_release);
}

static void drv_fibre_entry(xr_task_t *t)
{
	eb_driver_t *d = t->user;
	eb_t *b = d->b;

	for (int i = 0; i < d->ops; i++)
	{
		eb_conn_t *c = &b->conns[(d->id + i) % b->k];
		uint64_t t0 = xr_tsc();

		conn_post(c, d->id);
		xr_parker_unpark(&c->task.parker, 0);
		xr_task_wait(t);
		d->lat[i] = xr_tsc() - t0;
	}
	atomic_fetch_add_explicit(&b->drivers_done, 1, memory_order_release);
}

/* ---------- 统计 ---------- */

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static uint64_t pct_ns(const uint64_t *v, size_t n, double p)
{
	size_t i;

	if (n == 0)
	{
		return 0;
	}
	i = (size_t)(p * (double)(n - 1));
	return xr_tsc_to_ns(v[i]);
}

int main(int argc, char **argv)
{
	int k = 16;
	int m = 4;
	int ops = 20000;
	int wkcpu = 1;
	int drcpu = 2;
	unsigned flags = 0;
	int park_mode = 0;
	int l0_fibre = 0;
	eb_t b;
	uint64_t t_start;
	uint64_t t_end;
	uint64_t *all;
	size_t all_n = 0;
	xr_worker_stats_t sc;
	xr_worker_stats_t sd;
	xr_worker_stats_t zero = { 0 };

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--conns") == 0 && i + 1 < argc)
		{
			k = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--drivers") == 0 && i + 1 < argc)
		{
			m = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)
		{
			ops = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--wkcpu") == 0 && i + 1 < argc)
		{
			wkcpu = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--drcpu") == 0 && i + 1 < argc)
		{
			drcpu = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--flags") == 0 && i + 1 < argc)
		{
			flags = (unsigned)strtoul(argv[++i], NULL, 0);
		}
		else if (strcmp(argv[i], "--ack=park") == 0)
		{
			park_mode = 1;
		}
		else if (strcmp(argv[i], "--ack=spin") == 0)
		{
			park_mode = 0;
		}
		else if (strcmp(argv[i], "--l0=fibre") == 0)
		{
			l0_fibre = 1;
		}
		else if (strcmp(argv[i], "--l0=stackless") == 0)
		{
			l0_fibre = 0;
		}
	}

	memset(&b, 0, sizeof(b));
	b.k = k;
	b.m = m;
	b.ops = ops;
	b.park_mode = park_mode;
	b.flags = flags;
	xr_time_init();

	b.conns = calloc((size_t)k, sizeof(*b.conns));
	b.drivers = calloc((size_t)m, sizeof(*b.drivers));
	if (b.conns == NULL || b.drivers == NULL)
	{
		fprintf(stderr, "oom\n");
		return 1;
	}
	for (int i = 0; i < m; i++)
	{
		b.drivers[i].lat = calloc((size_t)ops, sizeof(uint64_t));
		if (b.drivers[i].lat == NULL)
		{
			fprintf(stderr, "oom\n");
			return 1;
		}
	}

	b.wc = xr_worker_create(wkcpu);
	if (b.wc == NULL)
	{
		fprintf(stderr, "worker create failed\n");
		return 1;
	}
	xr_worker_set_flags(b.wc, flags);
	if (park_mode != 0)
	{
		b.wd = xr_worker_create(drcpu);
		if (b.wd == NULL)
		{
			fprintf(stderr, "worker create failed\n");
			return 1;
		}
		xr_worker_set_flags(b.wd, flags);
	}

	for (int i = 0; i < k; i++)
	{
		b.conns[i].b = &b;
		b.conns[i].id = i;
		pthread_mutex_init(&b.conns[i].lock, NULL);
		if (l0_fibre != 0)
		{
			if (xr_task_init_fibre(&b.conns[i].task,
					       conn_fibre_entry, 0) != 0)
			{
				fprintf(stderr, "fibre init failed\n");
				return 1;
			}
			b.conns[i].task.user = &b.conns[i];
		}
		else
		{
			xr_task_init(&b.conns[i].task, conn_step, &b.conns[i]);
		}
		xr_task_spawn(b.wc, &b.conns[i].task);
	}
	for (int i = 0; i < m; i++)
	{
		b.drivers[i].b = &b;
		b.drivers[i].id = i;
		b.drivers[i].ops = ops;
		b.drivers[i].cpu =
			drcpu >= 0 ? (drcpu + i) % xr_cpu_count() : -1;
		if (park_mode != 0)
		{
			if (l0_fibre != 0)
			{
				if (xr_task_init_fibre(&b.drivers[i].task,
						       drv_fibre_entry, 0) != 0)
				{
					fprintf(stderr, "fibre init failed\n");
					return 1;
				}
				b.drivers[i].task.user = &b.drivers[i];
			}
			else
			{
				xr_task_init(&b.drivers[i].task, drv_step,
					     &b.drivers[i]);
			}
			xr_task_spawn(b.wd, &b.drivers[i].task);
		}
	}

	while (atomic_load_explicit(&b.conns_ready, memory_order_acquire) < k)
	{
		xr_cpu_relax();
	}

	t_start = xr_now_ns();
	if (park_mode != 0)
	{
		while (atomic_load_explicit(&b.drivers_done,
					    memory_order_acquire) < m)
		{
			xr_cpu_relax();
		}
	}
	else
	{
		pthread_t *th = calloc((size_t)m, sizeof(*th));

		if (th == NULL)
		{
			return 1;
		}
		for (int i = 0; i < m; i++)
		{
			if (pthread_create(&th[i], NULL, drv_thread,
					   &b.drivers[i]) != 0)
			{
				fprintf(stderr, "pthread_create failed\n");
				return 1;
			}
		}
		for (int i = 0; i < m; i++)
		{
			pthread_join(th[i], NULL);
		}
		free(th);
	}
	t_end = xr_now_ns();

	/* L0 收尾:停 conns(其在最后一批 ack 后已挂起) */
	if (l0_fibre != 0)
	{
		atomic_store_explicit(&b.stop, 1, memory_order_release);
		for (int i = 0; i < k; i++)
		{
			xr_parker_unpark(&b.conns[i].task.parker, 0);
		}
		while (atomic_load_explicit(&b.conns_done,
					    memory_order_acquire) < k)
		{
			xr_cpu_relax();
		}
	}

	xr_worker_stats(b.wc, &sc);
	if (b.wd != NULL)
	{
		xr_worker_stats(b.wd, &sd);
	}
	else
	{
		sd = zero;
	}

	/* 合并样本 */
	all = calloc((size_t)m * (size_t)ops, sizeof(uint64_t));
	if (all == NULL)
	{
		return 1;
	}
	for (int i = 0; i < m; i++)
	{
		memcpy(&all[all_n], b.drivers[i].lat,
		       (size_t)ops * sizeof(uint64_t));
		all_n += (size_t)ops;
	}
	qsort(all, all_n, sizeof(uint64_t), cmp_u64);

	{
		uint64_t total = (uint64_t)m * (uint64_t)ops;
		uint64_t writes = sc.wake_writes + sd.wake_writes;
		uint64_t futex = sc.wake_futex + sd.wake_futex;
		uint64_t runs = sc.runs + sd.runs;
		uint64_t delivers = sc.delivers + sd.delivers;
		uint64_t lat_sum = 0;
		uint64_t lat_avg;
		double sec = (double)(t_end - t_start) / 1e9;

		for (size_t i = 0; i < all_n; i++)
		{
			lat_sum += all[i];
		}
		lat_avg = all_n != 0 ? xr_tsc_to_ns(lat_sum / all_n) : 0;

		printf("bench_echo: conns=%d drivers=%d ops/thread=%d total=%" PRIu64
		       " wkcpu=%d ack=%s l0=%s flags=%u\n",
		       k, m, ops, total, wkcpu,
		       park_mode != 0 ? "park" : "spin",
		       l0_fibre != 0 ? "fibre" : "stackless", flags);
		printf("  rps=%.0f elapsed=%.1fms\n", (double)total / sec,
		       (double)(t_end - t_start) / 1e6);
		printf("  req->ack p50=%" PRIu64 "ns p99=%" PRIu64
		       "ns avg=%" PRIu64 "ns\n",
		       pct_ns(all, all_n, 0.50), pct_ns(all, all_n, 0.99),
		       lat_avg);
		printf("  transport/req: wake_writes=%.4f futex=%.4f\n",
		       (double)writes / (double)total,
		       (double)futex / (double)total);
		printf("  runs/req=%.4f delivers/req=%.4f parks/deliver=%.3f"
		       "  [runs wc=%" PRIu64 " wd=%" PRIu64 "]\n",
		       (double)runs / (double)total,
		       (double)delivers / (double)total,
		       delivers != 0 ? (double)runs / (double)delivers : 0.0,
		       sc.runs, sd.runs);
	}

	for (int i = 0; i < k; i++)
	{
		pthread_mutex_destroy(&b.conns[i].lock);
	}
	for (int i = 0; i < m; i++)
	{
		free(b.drivers[i].lat);
	}
	free(all);
	free(b.conns);
	free(b.drivers);
	xr_worker_destroy(b.wc);
	if (b.wd != NULL)
	{
		xr_worker_destroy(b.wd);
	}
	return 0;
}
