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
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * L0 仪器标定与创建/销毁成本(02 §0.5.2 对照):
 *  --mode=calib : 两栈间 xr_cpu_switch 纯切换(2 次/迭代),报 ns/switch;
 *  --mode=create: --l0=stackless|fibre 的 spawn→完成→销毁,报 ns/个 + RSS;
 *  --mode=ring  : N 个驻留节点 token-ring(主线程逐跳 unpark 并等待恢复),
 *                 报每跳 t0→t2 延迟分布 + RSS。N 增大时 stackful 需触碰 N 份
 *                 栈页(缓存/TLB 工作集),stackless 无栈页 → 黑盒 TLB 代理。
 *  --mode=wake  : 同线程"自我唤醒"成本(eventfd 写→epoll 返回→drain),
 *                 作为 D8 self-wake 的理论代理;
 *  --mode=migrate: fibre 跨线程迁移边界(切换易/所有权难):同一 ctx 由 A/B
 *                 交替 resume;报 pure resume(切换)与 wall(含跨线程交接);
 *                 并探测 local 栈变量/线程 id/TLS/线程局部地址的迁移语义。
 *   bench_l0 [--mode calib|create|ring|migrate] [--l0 stackless|fibre]
 *            [--ops N] [--fibres N] [--stack BYTES] [--wkcpu C] [--prodcpu C]
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

/* ---------- migrate:fibre 跨线程迁移边界(切换易/所有权难) ---------- */

static _Thread_local uint64_t mig_tls_val;
static _Thread_local int mig_tls_loop;

/* noinline 探针:防编译器把 pthread_self(const)/TLS 地址跨 suspend CSE。
 * 注意:release 下普通写法会被 CSE——这本身就是迁移风险(缓存值跨迁移失效)。 */
__attribute__((noinline)) static uint64_t mig_probe_tid(void)
{
	__asm__ __volatile__("" ::: "memory");
	return (uint64_t)pthread_self();
}

__attribute__((noinline)) static uintptr_t mig_probe_tls_addr(void)
{
	__asm__ __volatile__("" ::: "memory");
	return (uintptr_t)&mig_tls_loop;
}

__attribute__((noinline)) static uint64_t mig_probe_tls_val(void)
{
	__asm__ __volatile__("" ::: "memory");
	return mig_tls_val;
}

typedef struct
{
	xr_ctx_t *ctx;
	xr_task_t task;
	_Atomic uint64_t turn;
	uint64_t total; /* resume 总次数 = iters + 1 */
	uint64_t iters; /* suspend 次数 */
	uint64_t a_pure, a_n;
	uint64_t b_pure, b_n;
	/* 迁移语义探测 */
	uint64_t local_b, local_a;
	uintptr_t addr_b, addr_a;
	uint64_t tid_b, tid_a;
	uint64_t tls_b, tls_a;
	uintptr_t loop_b, loop_a;
} mig_t;

typedef struct
{
	mig_t *m;
	int id; /* 0=A,1=B */
} mig_arg_t;

static void mig_entry(xr_task_t *t)
{
	mig_t *m = t->user;
	uint64_t local = 0xDEADBEEFCAFEull;

	m->local_b = local;
	m->addr_b = (uintptr_t)&local;
	m->tid_b = mig_probe_tid();
	m->tls_b = mig_probe_tls_val();
	m->loop_b = mig_probe_tls_addr();
	for (uint64_t i = 0; i < m->iters; i++)
	{
		xr_ctx_suspend(m->ctx);
	}
	m->local_a = local;
	m->addr_a = (uintptr_t)&local;
	m->tid_a = mig_probe_tid();
	m->tls_a = mig_probe_tls_val();
	m->loop_a = mig_probe_tls_addr();
}

static void *mig_thread(void *arg)
{
	mig_arg_t *a = arg;
	mig_t *m = a->m;
	int me = a->id;
	uint64_t pure = 0;
	uint64_t n = 0;

	mig_tls_val = me == 0 ? 0xA11Aull : 0xB0Bull;
	mig_tls_loop = me;
	for (;;)
	{
		uint64_t t = atomic_load_explicit(&m->turn,
						  memory_order_acquire);
		uint64_t t0;

		if (t >= m->total)
		{
			break;
		}
		if ((int)(t & 1u) != me)
		{
			xr_cpu_relax();
			continue;
		}
		t0 = xr_tsc();
		xr_ctx_resume(m->ctx);
		pure += xr_tsc() - t0;
		n++;
		atomic_store_explicit(&m->turn, t + 1, memory_order_release);
	}
	if (me == 0)
	{
		m->a_pure = pure;
		m->a_n = n;
	}
	else
	{
		m->b_pure = pure;
		m->b_n = n;
	}
	return NULL;
}

static int run_migrate(uint64_t iters)
{
	mig_t m;
	mig_arg_t aa;
	mig_arg_t ab;
	pthread_t ta;
	pthread_t tb;
	uint64_t t_wall0, t_wall1;
	uint64_t pure;
	uint64_t n;
	uint64_t sum = 0;
	uint64_t sn = 0;

	memset(&m, 0, sizeof(m));
	m.iters = iters;
	m.total = iters + 1;
	m.task.user = &m;
	m.ctx = xr_ctx_create(&m.task, mig_entry, 0);
	if (m.ctx == NULL)
	{
		XR_LOGE("migrate ctx create failed");
		return 1;
	}

	/* 阶段 1:同线程 resume 基线(纯切换) */
	{
		uint64_t t0 = xr_now_ns();
		uint64_t t1;

		mig_tls_val = 0xAAAAull;
		mig_tls_loop = 42;
		xr_ctx_resume(m.ctx);
		while (xr_ctx_state(m.ctx) != XR_CTX_DONE)
		{
			uint64_t a = xr_tsc();

			xr_ctx_resume(m.ctx);
			sum += xr_tsc() - a;
			sn++;
		}
		t1 = xr_now_ns();
		printf("bench_l0 migrate: same-thread resumes=%" PRIu64
		       "  pure=%.1f ns/resume  wall=%.1f ns/resume\n",
		       sn + 1, (double)xr_tsc_to_ns(sum) / (double)sn,
		       (double)(t1 - t0) / (double)(sn + 1));
	}

	/* 阶段 2:跨线程迁移(A/B 交替 resume;pure=切换,wall=含交接) */
	memset(&m, 0, sizeof(m));
	m.iters = iters;
	m.total = iters + 1;
	m.task.user = &m;
	m.ctx = xr_ctx_create(&m.task, mig_entry, 0);
	if (m.ctx == NULL)
	{
		XR_LOGE("migrate ctx create failed");
		return 1;
	}
	aa.m = &m;
	aa.id = 0;
	ab.m = &m;
	ab.id = 1;
	t_wall0 = xr_now_ns();
	if (pthread_create(&ta, NULL, mig_thread, &aa) != 0 ||
	    pthread_create(&tb, NULL, mig_thread, &ab) != 0)
	{
		XR_LOGE("migrate pthread create failed");
		return 1;
	}
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
	t_wall1 = xr_now_ns();
	pure = m.a_pure + m.b_pure;
	n = m.a_n + m.b_n;
	printf("bench_l0 migrate: cross-thread resumes=%" PRIu64
	       "  pure=%.1f ns/resume  wall=%.1f ns/resume"
	       "  handoff≈%.1f ns\n",
	       n, (double)xr_tsc_to_ns(pure) / (double)n,
	       (double)(t_wall1 - t_wall0) / (double)n,
	       (double)(t_wall1 - t_wall0) / (double)n -
		       (double)xr_tsc_to_ns(pure) / (double)n);

	/* 阶段 3:迁移语义探测(iters=1,同一次挂起跨线程恢复) */
	{
		mig_t p;

		memset(&p, 0, sizeof(p));
		p.iters = 1;
		p.total = 2;
		p.task.user = &p;
		p.ctx = xr_ctx_create(&p.task, mig_entry, 0);
		if (p.ctx == NULL)
		{
			XR_LOGE("migrate ctx create failed");
			return 1;
		}
		aa.m = &p;
		aa.id = 0;
		ab.m = &p;
		ab.id = 1;
		if (pthread_create(&ta, NULL, mig_thread, &aa) != 0 ||
		    pthread_create(&tb, NULL, mig_thread, &ab) != 0)
		{
			XR_LOGE("migrate pthread create failed");
			return 1;
		}
		pthread_join(ta, NULL);
		pthread_join(tb, NULL);
		printf("bench_l0 migrate probe:\n");
		printf("  stack local : same=%d addr_same=%d (0x%" PRIx64 ")\n",
		       p.local_b == p.local_a, p.addr_b == p.addr_a,
		       p.local_a);
		printf("  thread id   : changed=%d (A=%" PRIu64 " B=%" PRIu64
		       ")\n",
		       p.tid_b != p.tid_a, p.tid_b, p.tid_a);
		printf("  TLS value   : changed=%d (before=0x%" PRIx64
		       " after=0x%" PRIx64 ") → 语义随线程\n",
		       p.tls_b != p.tls_a, p.tls_b, p.tls_a);
		printf("  TLS addr    : changed=%d → 缓存句柄/loop 指针必须重绑\n",
		       p.loop_b != p.loop_a);
		xr_ctx_destroy(p.ctx);
	}

	xr_ctx_destroy(m.ctx);
	return 0;
}

/* ---------- wake:同线程自我唤醒(eventfd write→epoll 返回→drain) ---------- */

static int run_wake(uint64_t iters)
{
	int epfd = epoll_create1(EPOLL_CLOEXEC);
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	struct epoll_event ev;
	struct epoll_event out;
	uint64_t sum = 0;
	uint64_t n = 0;
	uint64_t max = 0;

	if (epfd < 0 || efd < 0)
	{
		return 1;
	}
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.u64 = 1;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev) != 0)
	{
		return 1;
	}
	for (uint64_t i = 0; i < iters; i++)
	{
		uint64_t one = 1;
		uint64_t v;
		uint64_t a;
		uint64_t b;
		ssize_t r;

		a = xr_tsc();
		r = write(efd, &one, sizeof(one));
		(void)r;
		(void)epoll_wait(epfd, &out, 1, -1);
		r = read(efd, &v, sizeof(v));
		(void)r;
		b = xr_tsc();
		sum += b - a;
		if (b - a > max)
		{
			max = b - a;
		}
		n++;
	}
	printf("bench_l0 wake: iters=%" PRIu64 " self-wake avg=%.0fns"
	       " max=%.0fns (eventfd write+epoll return+drain)\n",
	       n, (double)xr_tsc_to_ns(sum) / (double)n,
	       (double)xr_tsc_to_ns(max));
	close(efd);
	close(epfd);
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
	if (strcmp(mode, "migrate") == 0)
	{
		return run_migrate(ops);
	}
	if (strcmp(mode, "wake") == 0)
	{
		return run_wake(ops);
	}
	XR_LOGE("unknown mode: %s", mode);
	return 2;
}
