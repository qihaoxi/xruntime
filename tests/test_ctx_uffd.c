#include "xr/xr_ctx.h"
#include "xr/xr_task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/*
 * U18:arena + UFFD-WP 保护页的溢出捕获验证。
 * 无 guard 的 arena 会因匿名映射合并而消除 VMA,但失去溢出保护;
 * ARENA_GUARD 用 UFFD-WP(不拆 VMA)把保护页写成只读,溢出写触发
 * WP fault → handler 计数并解除保护(写入重试成功)。
 * 需 unprivileged_userfaultfd=1 或 CAP_SYS_PTRACE;不可用时 SKIP。
 */

static int failures;
static _Atomic int done;

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

static void overflow_entry(xr_task_t *t)
{
	void *g = xr_ctx_guard_page(t->ctx);

	if (g != NULL)
	{
		volatile char *p = (volatile char *)g;

		*p = 1; /* 溢出写保护页 */
	}
	atomic_store_explicit(&done, 1, memory_order_release);
}

int main(void)
{
	xr_ctx_t *ctx;
	xr_task_t task;

	memset(&task, 0, sizeof(task));
	xr_ctx_set_alloc(XR_CTX_ALLOC_ARENA_GUARD, 0);
	ctx = xr_ctx_create(&task, overflow_entry, 0);
	if (ctx != NULL)
	{
		task.ctx = ctx; /* entry 经 t->ctx 取 guard 页 */
	}
	if (ctx == NULL)
	{
		fprintf(stderr, "ctx_uffd: create failed\n");
		return 1;
	}
	if (xr_ctx_guard_page(ctx) == NULL)
	{
		printf("ctx_uffd: SKIP (userfaultfd unavailable;"
		       " 需 sudo 或 vm.unprivileged_userfaultfd=1)\n");
		xr_ctx_destroy(ctx);
		return 0;
	}
	xr_ctx_resume(ctx); /* entry 溢出写 → WP fault → handler → 重试成功 */
	CHECK(xr_ctx_state(ctx) == XR_CTX_DONE);
	CHECK(atomic_load(&done) == 1);
	CHECK(xr_ctx_uffd_faults() >= 1);
	xr_ctx_destroy(ctx);
	if (failures != 0)
	{
		fprintf(stderr, "ctx_uffd: %d failure(s)\n", failures);
		return 1;
	}
	printf("ctx_uffd: OK (faults=%llu)\n",
	       (unsigned long long)xr_ctx_uffd_faults());
	return 0;
}
