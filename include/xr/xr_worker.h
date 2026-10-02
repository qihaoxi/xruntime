#ifndef XR_WORKER_H
#define XR_WORKER_H

#include "xr/xr_task.h"

#include <stdint.h>

/*
 * worker = 线程 + epoll(eventfd) + ready 队列 + registry(V0 基线)。
 * 当前只把 eventfd 接入 epoll,后续变体再加真实 IO 事件。
 */

typedef struct xr_worker xr_worker_t;
typedef struct xr_worker_stats
{
	uint64_t wake_writes; /* eventfd 写次数(合并后) */
	uint64_t delivers;    /* deliver 路径成功入队次数 */
	uint64_t runs;        /* step 执行次数(含 RUN_AGAIN 重排) */
} xr_worker_stats_t;

/* cpu<0 = 不绑核;失败返回 NULL。 */
xr_worker_t *xr_worker_create(int cpu);
void xr_worker_destroy(xr_worker_t *w);
void xr_worker_stats(xr_worker_t *w, xr_worker_stats_t *out);

#endif
