#ifndef XR_CTX_H
#define XR_CTX_H

#include <stddef.h>

/*
 * L0 栈切换抽象(02-upper-bound-design §0.5.4):resume/suspend 两个原语 + task
 * 状态。stackless 不需要 ctx(worker 直接调用 t->fn);stackful 后端 =
 * mmap 栈(带 guard 页) + 最小 x86-64 asm switch,入口在独立栈上运行,阻塞式
 * 等待经 suspend 让出栈。
 *
 * 状态机:
 *   RUNNABLE --resume--> [运行] --suspend--> PARKED --resume--> ...
 *                            \--entry 返回--> DONE(不可再 resume)
 *
 * 约束:
 * - ctx 归 owner worker 线程所有;resume/suspend 只在 owner 线程调用;
 * - deliver 侧只接触 parker/队列,不接触 ctx;
 * - 机制层(L1-L3)不得 include 本文件之外的栈实现(硬性,00 §1)。
 */

typedef struct xr_task xr_task_t;
typedef struct xr_ctx xr_ctx_t;
typedef void (*xr_ctx_entry_fn)(xr_task_t *t);

typedef enum
{
	XR_CTX_RUNNABLE = 0,
	XR_CTX_PARKED = 1,
	XR_CTX_DONE = 2,
} xr_ctx_state_t;

/* stack_size=0 → 默认 64KB(不含 guard);<16KB 取 16KB。失败返回 NULL。 */
xr_ctx_t *xr_ctx_create(xr_task_t *t, xr_ctx_entry_fn entry, size_t stack_size);
void xr_ctx_destroy(xr_ctx_t *c);
void xr_ctx_resume(xr_ctx_t *c);
void xr_ctx_suspend(xr_ctx_t *c);
xr_ctx_state_t xr_ctx_state(const xr_ctx_t *c);

/* asm 原语:保存当前 SP 到 *save_sp,切到 new_sp(实现见 xr_switch_x86_64.S)。 */
void xr_cpu_switch(void **save_sp, void *new_sp);

#endif
