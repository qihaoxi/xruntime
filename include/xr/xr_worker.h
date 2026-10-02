#ifndef XR_WORKER_H
#define XR_WORKER_H

#include "xr/xr_task.h"

#include <stdint.h>

/*
 * worker = 线程 + epoll(eventfd) + ready 队列 + registry(V0 基线)。
 * 当前只把 eventfd 接入 epoll,后续变体再加真实 IO 事件。
 */

typedef struct xr_worker xr_worker_t;

/* S3 变体开关(创建后、spawn 前设置;A/B 用) */
enum
{
	XR_WORKER_DIRECT = 1u << 0, /* V1:同线程安全点直投,跳过 transport */
	XR_WORKER_GATE = 1u << 1,   /* V2:sleeping 门控,未挂起不写 eventfd */
};

typedef struct xr_worker_stats
{
	uint64_t wake_writes; /* eventfd 写次数(合并后) */
	uint64_t delivers;    /* deliver 路径成功入队次数 */
	uint64_t runs;        /* step 执行次数(含 RUN_AGAIN 重排) */
	uint64_t wake_direct; /* V1 省掉的 transport 次数(同线程) */
	uint64_t wake_gated;  /* V2 省掉的 transport 次数(未挂起) */
} xr_worker_stats_t;

/* cpu<0 = 不绑核;失败返回 NULL。 */
xr_worker_t *xr_worker_create(int cpu);
void xr_worker_destroy(xr_worker_t *w);
void xr_worker_set_flags(xr_worker_t *w, unsigned flags);
void xr_worker_stats(xr_worker_t *w, xr_worker_stats_t *out);

#endif
