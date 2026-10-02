#ifndef XR_TASK_H
#define XR_TASK_H

#include "xr/xr_ctx.h"
#include "xr/xr_parker.h"

#include <stddef.h>
#include <stdint.h>

/*
 * stackless 续体(第一阶段不引入栈切换):task 的 fn 在 owner worker 线程上执行,
 * 需要等待时调用 xr_parker_park();返回码告诉 worker 如何处置:
 *   XR_TASK_DONE      - 结束,worker 从 registry 摘除;
 *   XR_TASK_PARKED    - 已挂起,等 deliver 入队后再跑;
 *   XR_TASK_RUN_AGAIN - 立即重排(消费到多余通知时)。
 *
 * V0 唤醒链(对齐 PEL 形态):unpark -> claim -> publish -> deliver
 *   -> registry(弱句柄 id)解析 -> ready 队列(mutex) -> 事件面(eventfd,
 *   libuv async-pending 式合并)。task 不直接暴露给唤醒方,唤醒方持 parker 指针,
 *   deliver 侧经 xr_waker_t{worker,id} 弱句柄解析,便于 V4 替换 lifetime 方案。
 *
 * V4a(XR_WORKER_WAKER_DIRECT):deliver ctx 直接为 task 指针,跳过 registry
 *   查找与注册。**生命期前置条件(硬性)**:task 对象必须活过所有 in-flight
 *   unpark(deliver 在 unpark 调用线程同步执行);沙盒内 task 为栈/静态对象,
 *   生产者 join 后才 destroy worker,满足该条件。真实运行时若 task 可提前
 *   销毁,需引用计数 waker(V4b)兜底。
 */

typedef struct xr_worker xr_worker_t;
typedef struct xr_task xr_task_t;
typedef int (*xr_task_fn)(xr_task_t *t);

enum
{
	XR_TASK_DONE = 0,
	XR_TASK_PARKED = 1,
	XR_TASK_RUN_AGAIN = 2,
};

typedef struct
{
	xr_worker_t *worker;
	uint64_t id;
} xr_waker_t;

struct xr_task
{
	xr_task_fn fn;
	void *user;
	xr_parker_t parker;
	xr_waker_t waker;
	xr_worker_t *owner;
	uint64_t id;
	xr_ctx_t *ctx;         /* L0 stackful:非 NULL 时 worker 走 ctx resume */
	xr_task_t *ready_next; /* ready 队列链 */
	xr_task_t *reg_next;   /* registry 桶链 */
};

void xr_task_init(xr_task_t *t, xr_task_fn fn, void *user);
void xr_task_spawn(xr_worker_t *w, xr_task_t *t);
xr_parker_t *xr_task_parker(xr_task_t *t);

/*
 * L0 stackful 任务:创建独立栈,entry 在 owner worker 线程的独立栈上运行;
 * entry 内用 xr_task_wait 阻塞等待唤醒,entry 返回即 DONE(worker 自动摘除
 * 并销毁 ctx)。创建失败返回 -1。仅在 xr_task_spawn 之前调用。
 */
int xr_task_init_fibre(xr_task_t *t, xr_ctx_entry_fn entry, size_t stack_size);

/*
 * stackful 阻塞式等待(仅 t->ctx != NULL):park 判定挂起 → 让出栈;
 * 恢复后消费 NOTIFIED 状态,与 stackless 的 step 重入语义等价。
 */
void xr_task_wait(xr_task_t *t);

#endif
