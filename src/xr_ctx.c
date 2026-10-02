#include "xr/xr_ctx.h"

#include "xr/xr_task.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * stackful L0 后端:mmap(guard 低页 + RW 栈) + xr_cpu_switch。
 * 首次 resume 进入伪造帧 → xr_ctx_trampoline → xr_ctx_entry_c。
 * 见 xr_ctx.h 状态机与所有权约束。
 */

#define XR_CTX_MIN_STACK (16u * 1024u)
#define XR_CTX_DEFAULT_STACK (64u * 1024u)

struct xr_ctx
{
	xr_task_t *task;
	xr_ctx_entry_fn entry;
	_Atomic int state;
	char *map;       /* mmap 基址(含 guard) */
	size_t map_size; /* guard + 栈 */
	char *stack_top; /* 16 对齐 */
	void *sp;        /* 任务栈保存点 */
	void *worker_sp; /* 进入前的 worker 栈保存点 */
};

/* asm 着陆点调用(符号在 xr_switch_x86_64.S 引用);原型抑制
 * -Wmissing-prototypes。entry 返回即 DONE,切回 worker 且不再恢复。 */
void xr_ctx_trampoline(void);
void xr_ctx_entry_c(xr_ctx_t *c);
void xr_ctx_entry_c(xr_ctx_t *c)
{
	c->entry(c->task);
	atomic_store_explicit(&c->state, XR_CTX_DONE, memory_order_release);
	xr_cpu_switch(&c->sp, c->worker_sp);
	abort(); /* DONE 后不得再 resume */
}

static size_t round_up(size_t n, size_t align)
{
	return (n + align - 1u) & ~(align - 1u);
}

xr_ctx_t *xr_ctx_create(xr_task_t *t, xr_ctx_entry_fn entry, size_t stack_size)
{
	xr_ctx_t *c;
	long page = sysconf(_SC_PAGESIZE);
	size_t sp_size;
	void **frame;

	if (page <= 0)
	{
		return NULL;
	}
	if (stack_size == 0)
	{
		stack_size = XR_CTX_DEFAULT_STACK;
	}
	if (stack_size < XR_CTX_MIN_STACK)
	{
		stack_size = XR_CTX_MIN_STACK;
	}
	sp_size = round_up(stack_size, (size_t)page);

	c = calloc(1, sizeof(*c));
	if (c == NULL)
	{
		return NULL;
	}
	c->map_size = (size_t)page + sp_size;
	c->map = mmap(NULL, c->map_size, PROT_NONE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (c->map == MAP_FAILED)
	{
		free(c);
		return NULL;
	}
	/* 低页 guard;高 sp_size 为可读写栈(向下增长,栈顶在高地址) */
	if (mprotect(c->map + page, sp_size, PROT_READ | PROT_WRITE) != 0)
	{
		munmap(c->map, c->map_size);
		free(c);
		return NULL;
	}
	c->task = t;
	c->entry = entry;
	c->stack_top = c->map + page + sp_size;
	atomic_store_explicit(&c->state, XR_CTX_RUNNABLE, memory_order_relaxed);

	/* 伪造帧:6 callee-saved + ret 地址(trampoline),r12=ctx。
	 * pop6+ret 后 rsp=stack_top(16 对齐),满足 call 前对齐要求。 */
	frame = (void **)(c->stack_top - 7 * sizeof(void *));
	frame[0] = NULL; /* r15 */
	frame[1] = NULL; /* r14 */
	frame[2] = NULL; /* r13 */
	frame[3] = c;    /* r12 → trampoline 取出为 rdi */
	frame[4] = NULL; /* rbx */
	frame[5] = NULL; /* rbp */
	frame[6] = (void *)xr_ctx_trampoline;
	c->sp = frame;
	return c;
}

void xr_ctx_destroy(xr_ctx_t *c)
{
	if (c == NULL)
	{
		return;
	}
	munmap(c->map, c->map_size);
	free(c);
}

void xr_ctx_resume(xr_ctx_t *c)
{
	if (atomic_load_explicit(&c->state, memory_order_acquire) == XR_CTX_DONE)
	{
		return;
	}
	atomic_store_explicit(&c->state, XR_CTX_RUNNABLE, memory_order_release);
	xr_cpu_switch(&c->worker_sp, c->sp);
}

void xr_ctx_suspend(xr_ctx_t *c)
{
	atomic_store_explicit(&c->state, XR_CTX_PARKED, memory_order_release);
	xr_cpu_switch(&c->sp, c->worker_sp);
	/* 恢复后返回调用方;state 已由 resume 置 RUNNABLE */
}

xr_ctx_state_t xr_ctx_state(const xr_ctx_t *c)
{
	return (xr_ctx_state_t)atomic_load_explicit(&c->state,
						    memory_order_acquire);
}
