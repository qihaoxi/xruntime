#ifndef XR_MPSC_H
#define XR_MPSC_H

#include "xr/xr_task.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * V3:Vyukov 有界 MPSC 环形队列(单消费者=owner worker,多生产者=deliver)。
 *
 * - `xr_mpsc_try_push`:无锁(fetch_add 取位 + slot seq release 发布)。
 *   先预检 tail-head>=capacity:满返回 false,由调用方走溢出链(worker lock
 *   下的链表)——**不中途放弃已 claim 的位**(放弃会留洞,消费者卡在该位);
 * - `xr_mpsc_pop`:仅消费者调用;head 仅消费者写,生产者 acquire 读;
 * - 依赖"同一 task 至多一个 ready 条目"不变式(parker 状态机保证,
 *   见 docs/00-unified-constraints.md §4.4);
 * - seq 初值=下标;pop 后置 pos+capacity,形成圈计数。
 */

typedef struct xr_mpsc xr_mpsc_t;

/* capacity 必须为 2 的幂;失败返回 NULL。 */
xr_mpsc_t *xr_mpsc_create(uint32_t capacity);
void xr_mpsc_destroy(xr_mpsc_t *m);

/* 入队:成功 true;满 false(调用方走溢出链)。 */
bool xr_mpsc_try_push(xr_mpsc_t *m, xr_task_t *t);

/* 出队:空返回 NULL。仅消费者(owner worker)调用。 */
xr_task_t *xr_mpsc_pop(xr_mpsc_t *m);

#endif
