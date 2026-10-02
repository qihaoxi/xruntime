#include "xr/xr_env.h"
#include "xr/xr_task.h"
#include "xr/xr_time.h"
#include "xr/xr_worker.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * L0 stackful 冒烟:park/unpark 跨线程、同线程双 fibre 互踢、批量创建销毁。
 * 与 stackless 的语义对照见 bench_l0;此处只验证上下文切换与生命期正确性。
 */

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

#define OPS 10000u
#define MANY 1000u
#define STOP UINT64_MAX

/* ---------- 跨线程 handoff:主线程 unpark,worker 上 fibre 恢复 ---------- */

typedef struct
{
	xr_task_t task;
	_Atomic uint64_t cur;
	_Atomic uint64_t expect;
	_Atomic int done;
	int err;
} handoff_t;

static void handoff_entry(xr_task_t *t)
{
	handoff_t *c = t->user;

	for (;;)
	{
		xr_task_wait(t);
		if (atomic_load_explicit(&c->cur, memory_order_acquire) == STOP)
		{
			break;
		}
		{
			uint64_t i = atomic_load_explicit(&c->cur,
							  memory_order_acquire);

			if (i != atomic_load_explicit(&c->expect,
						      memory_order_relaxed))
			{
				c->err = 1;
			}
			atomic_store_explicit(&c->expect, i + 1,
					      memory_order_release);
		}
	}
	CHECK(xr_ctx_state(t->ctx) == XR_CTX_RUNNABLE);
	atomic_store_explicit(&c->done, 1, memory_order_release);
}

static void test_handoff(void)
{
	handoff_t *c = calloc(1, sizeof(*c));
	xr_worker_t *w = xr_worker_create(-1);

	CHECK(c != NULL && w != NULL);
	if (c == NULL || w == NULL)
	{
		return;
	}
	CHECK(xr_task_init_fibre(&c->task, handoff_entry, 0) == 0);
	c->task.user = c;
	xr_task_spawn(w, &c->task);

	for (uint64_t i = 0; i < OPS; i++)
	{
		atomic_store_explicit(&c->cur, i, memory_order_release);
		xr_parker_unpark(&c->task.parker, i);
		while (atomic_load_explicit(&c->expect, memory_order_acquire) <=
		       i)
		{
			xr_cpu_relax();
		}
	}
	atomic_store_explicit(&c->cur, STOP, memory_order_release);
	xr_parker_unpark(&c->task.parker, 0);
	while (atomic_load_explicit(&c->done, memory_order_acquire) == 0)
	{
		xr_cpu_relax();
	}
	CHECK(c->err == 0);
	CHECK(atomic_load(&c->expect) == OPS);
	xr_worker_destroy(w);
	free(c);
}

/* ---------- 同线程双 fibre 互踢 ---------- */

typedef struct
{
	xr_task_t a;
	xr_task_t b;
	_Atomic uint64_t idx;
	_Atomic int stop;
	_Atomic int a_done;
	_Atomic int b_done;
} pingpong_t;

static void pp_a_entry(xr_task_t *t);
static void pp_b_entry(xr_task_t *t);

static void pp_a_entry(xr_task_t *t)
{
	pingpong_t *p = t->user;

	xr_task_wait(t); /* 主线程 kick */
	for (uint64_t i = 0; i < OPS; i++)
	{
		atomic_store_explicit(&p->idx, i, memory_order_release);
		xr_parker_unpark(&p->b.parker, i);
		xr_task_wait(t);
	}
	atomic_store_explicit(&p->stop, 1, memory_order_release);
	xr_parker_unpark(&p->b.parker, OPS);
	atomic_store_explicit(&p->a_done, 1, memory_order_release);
}

static void pp_b_entry(xr_task_t *t)
{
	pingpong_t *p = t->user;

	for (;;)
	{
		xr_task_wait(t);
		if (atomic_load_explicit(&p->stop, memory_order_acquire) != 0)
		{
			break;
		}
		CHECK(atomic_load_explicit(&p->idx, memory_order_acquire) <
		      OPS);
		xr_parker_unpark(&p->a.parker, 0);
	}
	atomic_store_explicit(&p->b_done, 1, memory_order_release);
}

static void test_pingpong(void)
{
	pingpong_t *p = calloc(1, sizeof(*p));
	xr_worker_t *w = xr_worker_create(-1);

	CHECK(p != NULL && w != NULL);
	if (p == NULL || w == NULL)
	{
		return;
	}
	CHECK(xr_task_init_fibre(&p->a, pp_a_entry, 16u * 1024u) == 0);
	CHECK(xr_task_init_fibre(&p->b, pp_b_entry, 16u * 1024u) == 0);
	p->a.user = p;
	p->b.user = p;
	xr_task_spawn(w, &p->a);
	xr_task_spawn(w, &p->b);
	xr_parker_unpark(&p->a.parker, 0);

	while (atomic_load_explicit(&p->a_done, memory_order_acquire) == 0 ||
	       atomic_load_explicit(&p->b_done, memory_order_acquire) == 0)
	{
		xr_cpu_relax();
	}
	CHECK(atomic_load(&p->idx) == OPS - 1);
	xr_worker_destroy(w);
	free(p);
}

/* ---------- 批量创建/等待/销毁 ---------- */

typedef struct
{
	xr_task_t task;
	_Atomic int done;
} many_t;

static many_t g_many[MANY];

static void many_entry(xr_task_t *t)
{
	many_t *m = t->user;

	xr_task_wait(t);
	atomic_store_explicit(&m->done, 1, memory_order_release);
}

static void test_many(void)
{
	xr_worker_t *w = xr_worker_create(-1);

	CHECK(w != NULL);
	if (w == NULL)
	{
		return;
	}
	for (int i = 0; i < (int)MANY; i++)
	{
		CHECK(xr_task_init_fibre(&g_many[i].task, many_entry,
					 16u * 1024u) == 0);
		g_many[i].task.user = &g_many[i];
		atomic_store(&g_many[i].done, 0);
		xr_task_spawn(w, &g_many[i].task);
	}
	for (int i = 0; i < (int)MANY; i++)
	{
		/* 首次运行后 fibre 已挂起或尚未运行:unpark 兼容 STORED */
		xr_parker_unpark(&g_many[i].task.parker, 0);
	}
	for (int i = 0; i < (int)MANY; i++)
	{
		while (atomic_load_explicit(&g_many[i].done,
					    memory_order_acquire) == 0)
		{
			xr_cpu_relax();
		}
	}
	xr_worker_destroy(w);
}

int main(void)
{
	test_handoff();
	test_pingpong();
	test_many();

	if (failures != 0)
	{
		fprintf(stderr, "ctx: %d failure(s)\n", failures);
		return 1;
	}
	printf("ctx: OK\n");
	return 0;
}
