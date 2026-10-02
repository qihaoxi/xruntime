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
#include <sys/mman.h>
#include <unistd.h>

/*
 * L0 仪器标定与创建/销毁成本(02 §0.5.2 对照):
 *  --mode=calib : 两栈间 xr_cpu_switch 纯切换(2 次/迭代),报 ns/switch;
 *  --mode=create: --l0=stackless|fibre 的 spawn→完成→销毁,报 ns/个 + RSS;
 *  --mode=ring  : N 个驻留节点 token-ring(主线程逐跳 unpark 并等待恢复),
 *                 报每跳 t0→t2 延迟分布 + RSS。N 增大时 stackful 需触碰 N 份
 *                 栈页(缓存/TLB 工作集),stackless 无栈页 → 黑盒 TLB 代理。
 *   bench_l0 [--mode calib|create|ring] [--l0 stackless|fibre] [--ops N]
 *            [--fibres N] [--stack BYTES] [--wkcpu C] [--prodcpu C]
 */

static _Atomic int calib_done;
static void *calib_sp;
static void *main_sp;
static uint64_t calib_iters;

static void calib_target(void);
static void calib_target(void)
{
	for (uint64_t i = 0; i < calib_iters; i++)
	{
		xr_cpu_switch(&calib_sp, main_sp);
	}
	atomic_store_explicit(&calib_done, 1, memory_order_release);
	xr_cpu_switch(&calib_sp, main_sp);
	abort(); /* main 不再切回 */
}

static void print_rss(const char *tag)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	int n = 0;

	if (f == NULL)
	{
		return;
	}
	printf("  rss[%s]:", tag);
	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (strncmp(line, "VmPeak:", 7) == 0 ||
		    strncmp(line, "VmRSS:", 6) == 0)
		{
			line[strcspn(line, "\n")] = '\0';
			printf(" %s;", line);
			n++;
		}
	}
	printf("%s\n", n == 0 ? " (unavailable)" : "");
	(void)fclose(f);
}

static int run_calib(uint64_t iters)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t sp_size = 64u * 1024u;
	size_t map_size = (size_t)page + sp_size;
	char *map = mmap(NULL, map_size, PROT_NONE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	void **frame;
	uint64_t t0, t1;

	if (map == MAP_FAILED)
	{
		XR_LOGE("calib mmap failed");
		return 1;
	}
	if (mprotect(map + page, sp_size, PROT_READ | PROT_WRITE) != 0)
	{
		munmap(map, map_size);
		return 1;
	}
	calib_iters = iters;
	frame = (void **)(map + page + sp_size - 7 * sizeof(void *));
	for (int i = 0; i < 6; i++)
	{
		frame[i] = NULL;
	}
	frame[6] = (void *)calib_target;
	calib_sp = frame;
	atomic_store(&calib_done, 0);

	t0 = xr_tsc();
	xr_cpu_switch(&main_sp, calib_sp);
	while (atomic_load_explicit(&calib_done, memory_order_acquire) == 0)
	{
		xr_cpu_switch(&main_sp, calib_sp);
	}
	t1 = xr_tsc();

	printf("bench_l0 calib: iters=%" PRIu64 " switches=%" PRIu64
	       "  %.1f ns/switch\n",
	       iters, 2 * iters + 2,
	       (double)xr_tsc_to_ns(t1 - t0) / (double)(2 * iters + 2));
	munmap(map, map_size);
	return 0;
}

/* ---------- create:spawn→完成(含 ctx 创建/销毁) ---------- */

typedef struct
{
	xr_task_t task;
	_Atomic int done;
} cr_t;

static int cr_fn(xr_task_t *t)
{
	cr_t *c = t->user;

	atomic_store_explicit(&c->done, 1, memory_order_release);
	return XR_TASK_DONE;
}

static void cr_entry(xr_task_t *t)
{
	cr_t *c = t->user;

	atomic_store_explicit(&c->done, 1, memory_order_release);
}

static int run_create(int l0_fibre, uint64_t n, size_t stack)
{
	cr_t *arr = calloc((size_t)n, sizeof(*arr));
	xr_worker_t *w = xr_worker_create(-1);
	uint64_t t0, t1;

	if (arr == NULL || w == NULL)
	{
		XR_LOGE("create setup failed");
		return 1;
	}
	t0 = xr_tsc();
	for (uint64_t i = 0; i < n; i++)
	{
		if (l0_fibre != 0)
		{
			if (xr_task_init_fibre(&arr[i].task, cr_entry, stack) !=
			    0)
			{
				XR_LOGE("fibre init failed at %" PRIu64, i);
				return 1;
			}
			arr[i].task.user = &arr[i];
		}
		else
		{
			xr_task_init(&arr[i].task, cr_fn, &arr[i]);
		}
		xr_task_spawn(w, &arr[i].task);
	}
	for (uint64_t i = 0; i < n; i++)
	{
		while (atomic_load_explicit(&arr[i].done,
					    memory_order_acquire) == 0)
		{
			xr_cpu_relax();
		}
	}
	/* fibre:entry 返回后由 worker destroy ctx;done 只覆盖 entry 返回前 */
	{
		xr_worker_stats_t st;

		do
		{
			xr_worker_stats(w, &st);
		} while (st.runs < n);
	}
	usleep(1000); /* 让最后一个 ctx destroy 完成 */
	t1 = xr_tsc();

	printf("bench_l0 create: l0=%s n=%" PRIu64 " stack=%zu  %.1f ns/task\n",
	       l0_fibre != 0 ? "fibre" : "stackless", n, stack,
	       (double)xr_tsc_to_ns(t1 - t0) / (double)n);
	print_rss(l0_fibre != 0 ? "fibre" : "stackless");

	xr_worker_destroy(w);
	free(arr);
	return 0;
}

/* ---------- ring:N 驻留节点 token-ring(黑盒 TLB/驻留代理) ---------- */

typedef struct ring_stat ring_stat_t;

typedef struct
{
	xr_task_t task;
	ring_stat_t *s;
	int id;
	_Atomic uint64_t count;
} ring_node_t;

struct ring_stat
{
	int n;
	ring_node_t *nodes;
	_Atomic int stop;
	_Atomic int done_nodes;
	_Atomic uint64_t slot;
	uint64_t *t0;
	uint64_t *t1;
	uint64_t *t2;
	uint64_t total; /* 采样跳数 */
};

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

static int ring_step(xr_task_t *t)
{
	ring_node_t *nd = t->user;
	ring_stat_t *s = nd->s;

	for (;;)
	{
		xr_park_result_t r = xr_parker_park(&t->parker);

		if (r == XR_PARK_SUSPENDED)
		{
			return XR_TASK_PARKED;
		}
		if (atomic_load_explicit(&s->stop, memory_order_acquire) != 0)
		{
			atomic_fetch_add_explicit(&s->done_nodes, 1,
						  memory_order_release);
			return XR_TASK_DONE;
		}
		s->t2[atomic_load_explicit(&s->slot, memory_order_acquire)] =
			xr_tsc();
		atomic_fetch_add_explicit(&nd->count, 1, memory_order_release);
		if (xr_parker_park(&t->parker) == XR_PARK_SUSPENDED)
		{
			return XR_TASK_PARKED;
		}
	}
}

static void ring_entry(xr_task_t *t)
{
	ring_node_t *nd = t->user;
	ring_stat_t *s = nd->s;

	for (;;)
	{
		xr_task_wait(t);
		if (atomic_load_explicit(&s->stop, memory_order_acquire) != 0)
		{
			atomic_fetch_add_explicit(&s->done_nodes, 1,
						  memory_order_release);
			return;
		}
		s->t2[atomic_load_explicit(&s->slot, memory_order_acquire)] =
			xr_tsc();
		atomic_fetch_add_explicit(&nd->count, 1, memory_order_release);
	}
}

static int run_ring(int l0_fibre, int n, uint64_t sample_target,
		    size_t stack)
{
	ring_stat_t s;
	xr_worker_t *w = xr_worker_create(-1);
	uint64_t rounds;
	uint64_t *producer = NULL;
	uint64_t *consumer = NULL;
	uint64_t *rtt = NULL;
	uint64_t neg = 0;
	uint64_t t_start, t_end;

	if (w == NULL || n <= 0)
	{
		return 1;
	}
	rounds = sample_target / (uint64_t)n;
	if (rounds < 2)
	{
		rounds = 2;
	}
	memset(&s, 0, sizeof(s));
	s.n = n;
	s.total = (uint64_t)n * rounds;
	s.nodes = calloc((size_t)n, sizeof(*s.nodes));
	s.t0 = calloc((size_t)s.total + (size_t)n, sizeof(uint64_t));
	s.t1 = calloc((size_t)s.total + (size_t)n, sizeof(uint64_t));
	s.t2 = calloc((size_t)s.total + (size_t)n, sizeof(uint64_t));
	producer = malloc((size_t)s.total * sizeof(uint64_t));
	consumer = malloc((size_t)s.total * sizeof(uint64_t));
	rtt = malloc((size_t)s.total * sizeof(uint64_t));
	if (s.nodes == NULL || s.t0 == NULL || s.t1 == NULL || s.t2 == NULL ||
	    producer == NULL || consumer == NULL || rtt == NULL)
	{
		XR_LOGE("ring oom");
		return 1;
	}

	for (int i = 0; i < n; i++)
	{
		s.nodes[i].s = &s;
		s.nodes[i].id = i;
		if (l0_fibre != 0)
		{
			if (xr_task_init_fibre(&s.nodes[i].task, ring_entry,
					       stack) != 0)
			{
				XR_LOGE("ring fibre init failed");
				return 1;
			}
			s.nodes[i].task.user = &s.nodes[i];
		}
		else
		{
			xr_task_init(&s.nodes[i].task, ring_step, &s.nodes[i]);
		}
		xr_task_spawn(w, &s.nodes[i].task);
	}
	print_rss(l0_fibre != 0 ? "ring-fibre-reside" : "ring-stackless-reside");

	/* warmup:每节点 1 跳(slot 用追加区,不采样) */
	for (int i = 0; i < n; i++)
	{
		atomic_store_explicit(&s.slot, s.total + (uint64_t)i,
				      memory_order_release);
		xr_parker_unpark(&s.nodes[i].task.parker, 0);
		while (atomic_load_explicit(&s.nodes[i].count,
					    memory_order_acquire) < 1)
		{
			xr_cpu_relax();
		}
	}

	t_start = xr_tsc();
	for (uint64_t r = 0; r < rounds; r++)
	{
		for (int i = 0; i < n; i++)
		{
			uint64_t slot = r * (uint64_t)n + (uint64_t)i;
			uint64_t t0, t1;

			atomic_store_explicit(&s.slot, slot,
					      memory_order_release);
			t0 = xr_tsc();
			xr_parker_unpark(&s.nodes[i].task.parker, 0);
			t1 = xr_tsc();
			s.t0[slot] = t0;
			s.t1[slot] = t1;
			while (atomic_load_explicit(&s.nodes[i].count,
						    memory_order_acquire) <
			       r + 2)
			{
				xr_cpu_relax();
			}
		}
	}
	t_end = xr_tsc();

	atomic_store_explicit(&s.stop, 1, memory_order_release);
	for (int i = 0; i < n; i++)
	{
		xr_parker_unpark(&s.nodes[i].task.parker, 0);
	}
	while (atomic_load_explicit(&s.done_nodes, memory_order_acquire) < n)
	{
		xr_cpu_relax();
	}

	{
		uint64_t m = 0;
		uint64_t sum_rtt = 0;

		for (uint64_t i = 0; i < s.total; i++)
		{
			int64_t d1 = (int64_t)(s.t2[i] - s.t0[i]);
			int64_t d2 = (int64_t)(s.t2[i] - s.t1[i]);

			if (d1 < 0 || d2 < 0)
			{
				neg++;
				d1 = d1 < 0 ? 0 : d1;
				d2 = d2 < 0 ? 0 : d2;
			}
			producer[m] = s.t1[i] - s.t0[i];
			consumer[m] = (uint64_t)d2;
			rtt[m] = (uint64_t)d1;
			sum_rtt += (uint64_t)d1;
			m++;
		}
		qsort(producer, m, sizeof(uint64_t), cmp_u64);
		qsort(consumer, m, sizeof(uint64_t), cmp_u64);
		qsort(rtt, m, sizeof(uint64_t), cmp_u64);
		printf("bench_l0 ring: l0=%s fibres=%d rounds=%" PRIu64
		       " hops=%" PRIu64 " stack=%zu neg=%" PRIu64 "\n",
		       l0_fibre != 0 ? "fibre" : "stackless", n, rounds, m,
		       stack, neg);
		printf("  producer p50=%" PRIu64 "ns  consumer p50=%" PRIu64
		       "ns  rtt p50=%" PRIu64 "ns p90=%" PRIu64
		       "ns p99=%" PRIu64 "ns avg=%" PRIu64 "ns\n",
		       pct_ns(producer, m, 0.50), pct_ns(consumer, m, 0.50),
		       pct_ns(rtt, m, 0.50), pct_ns(rtt, m, 0.90),
		       pct_ns(rtt, m, 0.99),
		       xr_tsc_to_ns(sum_rtt / m));
		printf("  elapsed=%.1fms  hops/s=%.0f\n",
		       (double)(t_end - t_start) / 1e6,
		       (double)m / ((double)(t_end - t_start) / 1e9));
	}
	print_rss(l0_fibre != 0 ? "ring-fibre-end" : "ring-stackless-end");

	xr_worker_destroy(w);
	free(rtt);
	free(consumer);
	free(producer);
	free(s.t2);
	free(s.t1);
	free(s.t0);
	free(s.nodes);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = "calib";
	int l0_fibre = 0;
	uint64_t ops = 1000000;
	int fibres = 1024;
	size_t stack = 0;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
		{
			mode = argv[++i];
		}
		else if (strcmp(argv[i], "--l0") == 0 && i + 1 < argc)
		{
			l0_fibre = strcmp(argv[++i], "fibre") == 0;
		}
		else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)
		{
			ops = strtoull(argv[++i], NULL, 0);
		}
		else if (strcmp(argv[i], "--fibres") == 0 && i + 1 < argc)
		{
			fibres = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--stack") == 0 && i + 1 < argc)
		{
			stack = (size_t)strtoull(argv[++i], NULL, 0);
		}
	}

	xr_time_init();
	if (strcmp(mode, "calib") == 0)
	{
		return run_calib(ops);
	}
	if (strcmp(mode, "create") == 0)
	{
		return run_create(l0_fibre, ops, stack);
	}
	if (strcmp(mode, "ring") == 0)
	{
		return run_ring(l0_fibre, fibres, ops, stack);
	}
	XR_LOGE("unknown mode: %s", mode);
	return 2;
}
