#ifndef XR_PARKER_H
#define XR_PARKER_H

#include <stdatomic.h>
#include <stdint.h>

/*
 * 三态 parker(形态对齐 PEL doc00 §5.2 / doc97,去掉 fibrage/registry 耦合,
 * 便于单变量替换唤醒链各段):
 *
 *   state: IDLE --park CAS--> PARKED --unpark CAS--> NOTIFIED --park CAS--> IDLE
 *                \--unpark CAS--> NOTIFIED(提前通知,无 waiter)
 *
 * - park:  返回 CONSUMED(已有通知,不挂起)或 SUSPENDED(已置 PARKED,调用方
 *          负责挂起;唤醒侧走 DELIVER 时回调 deliver);
 * - unpark:单发布者(pub 自旋锁),先写 payload 再迁状态:
 *            IDLE→NOTIFIED   = STORED(无等待者,仅存证);
 *            PARKED→NOTIFIED = DELIVER(回调 deliver,调用方在回调里把等待者
 *                              变 runnable/投递队列);
 *            NOTIFIED        = MERGED(已有未消费通知,不重复唤醒)。
 * - deliver 在 unpark 调用线程、持有 pub 时执行,不得重入本 parker;
 * - 同一 parker 单等待者(park 侧独占),unpark 多生产者并发安全。
 *
 * 测量点(S2):pub CAS / payload / state CAS / deliver(registry+queue+wake)。
 */

typedef enum
{
	XR_PARK_IDLE = 0,
	XR_PARK_PARKED = 1,
	XR_PARK_NOTIFIED = 2,
} xr_parker_state_t;

typedef enum
{
	XR_PARK_CONSUMED = 0,  /* 消费已有通知,未挂起 */
	XR_PARK_SUSPENDED = 1, /* 已置 PARKED,调用方挂起等待 DELIVER */
} xr_park_result_t;

typedef enum
{
	XR_UNPARK_STORED = 0,  /* IDLE→NOTIFIED,无等待者 */
	XR_UNPARK_DELIVER = 1, /* PARKED→NOTIFIED,已回调 deliver */
	XR_UNPARK_MERGED = 2,  /* 已有未消费通知,合并 */
} xr_unpark_result_t;

typedef struct xr_parker xr_parker_t;
typedef void (*xr_parker_deliver_fn)(xr_parker_t *p, uint64_t payload, void *ctx);

struct xr_parker
{
	_Atomic uint32_t state;
	_Atomic uint32_t pub;
	_Atomic uint64_t payload;
	xr_parker_deliver_fn deliver;
	void *ctx;
};

void xr_parker_init(xr_parker_t *p, xr_parker_deliver_fn fn, void *ctx);
xr_park_result_t xr_parker_park(xr_parker_t *p);
xr_unpark_result_t xr_parker_unpark(xr_parker_t *p, uint64_t payload);
xr_parker_state_t xr_parker_state(const xr_parker_t *p);

#endif
