#ifndef XR_WORKER_H
#define XR_WORKER_H

#include "xr/xr_task.h"

#include <stdint.h>

/*
 * worker = 线程 + epoll(eventfd) + ready 队列 + registry(V0 基线)。
 * 当前只把 eventfd 接入 epoll,后续变体再加真实 IO 事件。
 */

typedef struct xr_worker xr_worker_t;

/* S3/S4 变体开关(创建后、spawn 前设置;A/B 用) */
enum
{
	XR_WORKER_DIRECT = 1u << 0,	/* V1:同线程安全点直投,跳过 transport */
	XR_WORKER_GATE = 1u << 1,	/* V2:sleeping 门控,未挂起不写 eventfd */
	XR_WORKER_MPSC = 1u << 2,	/* V3:lock-free MPSC ready 队列(预留) */
	XR_WORKER_WAKER_DIRECT = 1u << 3, /* V4a:直接 waker(跳过 registry 查找) */
	XR_WORKER_FUTEX = 1u << 5,	/* V5:futex 睡眠/唤醒(替代 epoll+eventfd) */
};

/*
 * 契约:GATE 依赖 mutex 队列对"入队 vs 睡眠前复核"的串行化;MPSC 模式下
 * 该协议失效(无锁 store-load 竞速),FUTEX 模式自带单字门控(sleeping 不再
 * 维护),故 GATE 与二者同开时自动忽略;FUTEX|MPSC 组合正确(单字协议
 * 不依赖队列串行化)。
 */

typedef struct xr_worker_stats
{
	uint64_t wake_writes; /* eventfd 写次数(合并后) */
	uint64_t delivers;    /* deliver 路径成功入队次数 */
	uint64_t runs;        /* step 执行次数(含 RUN_AGAIN 重排) */
	uint64_t wake_direct; /* V1 省掉的 transport 次数(同线程) */
	uint64_t wake_gated;  /* V2 省掉的 transport 次数(未挂起) */
	uint64_t wake_futex;  /* V5 futex_wake 次数 */
} xr_worker_stats_t;

/* cpu<0 = 不绑核;失败返回 NULL。 */
xr_worker_t *xr_worker_create(int cpu);
void xr_worker_destroy(xr_worker_t *w);
void xr_worker_set_flags(xr_worker_t *w, unsigned flags);
void xr_worker_stats(xr_worker_t *w, xr_worker_stats_t *out);

#endif
