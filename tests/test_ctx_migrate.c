#include "xr/xr_ctx.h"
#include "xr/xr_task.h"
#include "xr/xr_time.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * U11:fibre 跨线程迁移边界验证。
 * - 切换层:挂起的 fibre 由另一线程 resume 机制上可行(栈在进程 VA);
 * - 语义层:栈局部变量/地址跨迁移保持;线程 id、TLS 值、TLS 地址随线程变化
 *   (缓存 loop/句柄指针必须重绑,否则指向旧线程的 TLS)。
 * 自定义栈与 sanitizer 不兼容 → 仅 sanitizer=none 门禁注册。
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

static _Thread_local uint64_t tl_val;
static _Thread_local int tl_loop;

/* noinline 探针:防 release 下 pthread_self(const)/TLS 地址被跨 suspend CSE */
__attribute__((noinline)) static uint64_t probe_tid(void)
{
	__asm__ __volatile__("" ::: "memory");
	return (uint64_t)pthread_self();
}

__attribute__((noinline)) static uintptr_t probe_tls_addr(void)
{
	__asm__ __volatile__("" ::: "memory");
	return (uintptr_t)&tl_loop;
}

__attribute__((noinline)) static uint64_t probe_tls_val(void)
{
	__asm__ __volatile__("" ::: "memory");
	return tl_val;
}

typedef struct
{
	xr_ctx_t *ctx;
	xr_task_t task;
	_Atomic uint64_t turn;
	uint64_t total;
	uint64_t iters;
	uint64_t local_b, local_a;
	uintptr_t addr_b, addr_a;
	uint64_t tid_b, tid_a;
	uint64_t tls_b, tls_a;
	uintptr_t loop_b, loop_a;
} mig_t;

typedef struct
{
	mig_t *m;
	int id;
} marg_t;

static void mig_entry(xr_task_t *t)
{
	mig_t *m = t->user;
	uint64_t local = 0xDEADBEEFCAFEull;

	m->local_b = local;
	m->addr_b = (uintptr_t)&local;
	m->tid_b = probe_tid();
	m->tls_b = probe_tls_val();
	m->loop_b = probe_tls_addr();
	for (uint64_t i = 0; i < m->iters; i++)
	{
		xr_ctx_suspend(m->ctx);
	}
	m->local_a = local;
	m->addr_a = (uintptr_t)&local;
	m->tid_a = probe_tid();
	m->tls_a = probe_tls_val();
	m->loop_a = probe_tls_addr();
}

static void *mig_thread(void *arg)
{
	marg_t *a = arg;
	mig_t *m = a->m;

	tl_val = a->id == 0 ? 0xA11Aull : 0xB0Bull;
	tl_loop = a->id;
	for (;;)
	{
		uint64_t t = atomic_load_explicit(&m->turn,
						  memory_order_acquire);

		if (t >= m->total)
		{
			break;
		}
		if ((int)(t & 1u) != a->id)
		{
			xr_cpu_relax();
			continue;
		}
		xr_ctx_resume(m->ctx);
		atomic_store_explicit(&m->turn, t + 1, memory_order_release);
	}
	return NULL;
}

static void run_cross(mig_t *m)
{
	marg_t aa = { m, 0 };
	marg_t ab = { m, 1 };
	pthread_t ta;
	pthread_t tb;

	CHECK(pthread_create(&ta, NULL, mig_thread, &aa) == 0);
	CHECK(pthread_create(&tb, NULL, mig_thread, &ab) == 0);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
}

/* 纯原子 body 的跨线程迁移:应完整跑通(切换层无所有权检查) */
static void test_cross_atomic(void)
{
	mig_t m;

	memset(&m, 0, sizeof(m));
	m.iters = 1000;
	m.total = m.iters + 1;
	m.task.user = &m;
	m.ctx = xr_ctx_create(&m.task, mig_entry, 0);
	CHECK(m.ctx != NULL);
	if (m.ctx == NULL)
	{
		return;
	}
	run_cross(&m);
	CHECK(m.turn == m.total);
	CHECK(xr_ctx_state(m.ctx) == XR_CTX_DONE);
	CHECK(m.local_a == 0xDEADBEEFCAFEull);
	CHECK(m.addr_a == m.addr_b);
	xr_ctx_destroy(m.ctx);
}

/* 迁移语义边界:栈保持;线程身份/TLS/TLS 地址随线程变化 */
static void test_boundary(void)
{
	mig_t m;

	memset(&m, 0, sizeof(m));
	m.iters = 1;
	m.total = 2;
	m.task.user = &m;
	m.ctx = xr_ctx_create(&m.task, mig_entry, 0);
	CHECK(m.ctx != NULL);
	if (m.ctx == NULL)
	{
		return;
	}
	run_cross(&m);

	CHECK(m.local_b == m.local_a);
	CHECK(m.addr_b == m.addr_a);
	CHECK(m.tid_b != m.tid_a);
	CHECK(m.tls_b == 0xA11Aull);
	CHECK(m.tls_a == 0xB0Bull);
	CHECK(m.loop_b != m.loop_a);
	xr_ctx_destroy(m.ctx);
}

/* 同线程对照:TLS/线程 id 不变 */
static void test_same_thread(void)
{
	mig_t m;

	memset(&m, 0, sizeof(m));
	m.iters = 1;
	m.total = 2;
	m.task.user = &m;
	m.ctx = xr_ctx_create(&m.task, mig_entry, 0);
	CHECK(m.ctx != NULL);
	if (m.ctx == NULL)
	{
		return;
	}
	tl_val = 0xAAAAull;
	tl_loop = 42;
	xr_ctx_resume(m.ctx);
	CHECK(xr_ctx_state(m.ctx) == XR_CTX_PARKED);
	xr_ctx_resume(m.ctx);
	CHECK(xr_ctx_state(m.ctx) == XR_CTX_DONE);
	CHECK(m.tid_b == m.tid_a);
	CHECK(m.tls_b == 0xAAAAull);
	CHECK(m.tls_a == 0xAAAAull);
	CHECK(m.loop_b == m.loop_a);
	xr_ctx_destroy(m.ctx);
}

int main(void)
{
	test_cross_atomic();
	test_boundary();
	test_same_thread();

	if (failures != 0)
	{
		fprintf(stderr, "ctx_migrate: %d failure(s)\n", failures);
		return 1;
	}
	printf("ctx_migrate: OK\n");
	return 0;
}
