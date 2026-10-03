#ifndef XR_CTX_H
#define XR_CTX_H

#include <stddef.h>
#include <stdint.h>

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

/*
 * 栈分配模式(U18):
 * - MMAP:每 ctx 一次 mmap(guard+RW),2 VMA/个;默认。
 * - ARENA:进程级 arena bump(无 guard)。相邻匿名映射由内核合并 → VMA≈O(1);
 *   创建 ~ns;代价=栈溢出无保护(会静默踩相邻栈)。
 * - ARENA_GUARD:arena + UFFD-WP 保护页(不拆 VMA);溢出写触发 WP fault 由
 *   内部 handler 捕获(计数并解除保护,写入重试成功)。需 unprivileged
 *   userfaultfd 或 CAP_SYS_PTRACE;不可用时自动降级为 ARENA。
 */
typedef enum
{
	XR_CTX_ALLOC_MMAP = 0,
	XR_CTX_ALLOC_ARENA = 1,
	XR_CTX_ALLOC_ARENA_GUARD = 2,
} xr_ctx_alloc_t;

/* 进程级设置(创建 ctx 前;arena_bytes=0 → 默认 4GB VA,按需增长)。 */
void xr_ctx_set_alloc(xr_ctx_alloc_t mode, size_t arena_bytes);

/* 返回 UFFD-WP 保护页地址(仅 ARENA_GUARD 且 UFFD 可用),否则 NULL。 */
void *xr_ctx_guard_page(xr_ctx_t *c);

/* 诊断:UFFD-WP 溢出捕获次数。 */
uint64_t xr_ctx_uffd_faults(void);

/* stack_size=0 → 默认 64KB(不含 guard);<16KB 取 16KB。失败返回 NULL。 */
xr_ctx_t *xr_ctx_create(xr_task_t *t, xr_ctx_entry_fn entry, size_t stack_size);
void xr_ctx_destroy(xr_ctx_t *c);
void xr_ctx_resume(xr_ctx_t *c);
void xr_ctx_suspend(xr_ctx_t *c);
xr_ctx_state_t xr_ctx_state(const xr_ctx_t *c);

/* asm 原语:保存当前 SP 到 *save_sp,切到 new_sp(实现见 xr_switch_x86_64.S)。 */
void xr_cpu_switch(void **save_sp, void *new_sp);

#endif
