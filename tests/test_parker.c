#include "xr/xr_parker.h"
#include "xr/xr_time.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static int failures;

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

typedef struct
{
	_Atomic int count;
	_Atomic uint64_t last;
} deliver_box_t;

static void on_deliver(xr_parker_t *p, uint64_t payload, void *ctx)
{
	deliver_box_t *b = ctx;
	(void)p;
	atomic_fetch_add_explicit(&b->count, 1, memory_order_relaxed);
	atomic_store_explicit(&b->last, payload, memory_order_relaxed);
}

/* 挂起后被唤醒:DELIVER 一次,再 park 消费掉存证 */
static void test_park_then_deliver(void)
{
	xr_parker_t p;
	deliver_box_t b = { 0 };

	xr_parker_init(&p, on_deliver, &b);
	CHECK(xr_parker_park(&p) == XR_PARK_SUSPENDED);
	CHECK(xr_parker_unpark(&p, 42) == XR_UNPARK_DELIVER);
	CHECK(atomic_load(&b.count) == 1);
	CHECK(atomic_load(&b.last) == 42);
	CHECK(xr_parker_park(&p) == XR_PARK_CONSUMED);
	CHECK(xr_parker_park(&p) == XR_PARK_SUSPENDED);
}

/* 先通知后 park:无 waiter,STORED;park 直接消费 */
static void test_notify_before_park(void)
{
	xr_parker_t p;
	deliver_box_t b = { 0 };

	xr_parker_init(&p, on_deliver, &b);
	CHECK(xr_parker_unpark(&p, 7) == XR_UNPARK_STORED);
	CHECK(atomic_load(&b.count) == 0);
	CHECK(xr_parker_park(&p) == XR_PARK_CONSUMED);
	CHECK(atomic_load(&b.count) == 0);
	CHECK(xr_parker_park(&p) == XR_PARK_SUSPENDED);
}

/* 未消费期间多次 unpark 合并为单次通知 */
static void test_merge(void)
{
	xr_parker_t p;
	deliver_box_t b = { 0 };

	xr_parker_init(&p, on_deliver, &b);
	CHECK(xr_parker_unpark(&p, 1) == XR_UNPARK_STORED);
	CHECK(xr_parker_unpark(&p, 2) == XR_UNPARK_MERGED);
	CHECK(xr_parker_unpark(&p, 3) == XR_UNPARK_MERGED);
	CHECK(atomic_load(&b.count) == 0);
	CHECK(xr_parker_park(&p) == XR_PARK_CONSUMED);
	CHECK(xr_parker_park(&p) == XR_PARK_SUSPENDED);
}

typedef struct
{
	xr_parker_t p;
	_Atomic int parked;
	_Atomic int resumed;
} mt_ctx_t;

static void mt_deliver(xr_parker_t *p, uint64_t payload, void *ctx)
{
	mt_ctx_t *m = ctx;
	(void)p;
	(void)payload;
	atomic_store_explicit(&m->resumed, 1, memory_order_release);
}

static void *mt_waiter(void *arg)
{
	mt_ctx_t *m = arg;

	CHECK(xr_parker_park(&m->p) == XR_PARK_SUSPENDED);
	atomic_store_explicit(&m->parked, 1, memory_order_release);
	while (atomic_load_explicit(&m->resumed, memory_order_acquire) == 0)
	{
		xr_cpu_relax();
	}
	return NULL;
}

/* 跨线程:waiter 已 PARKED 时并发 unpark,仅首个 DELIVER,其余合并 */
static void test_cross_thread(void)
{
	mt_ctx_t m = { 0 };
	pthread_t th;
	int delivers = 0;

	xr_parker_init(&m.p, mt_deliver, &m);
	CHECK(pthread_create(&th, NULL, mt_waiter, &m) == 0);
	while (atomic_load_explicit(&m.parked, memory_order_acquire) == 0)
	{
		xr_cpu_relax();
	}
	for (int i = 0; i < 1000; i++)
	{
		if (xr_parker_unpark(&m.p, (uint64_t)i) == XR_UNPARK_DELIVER)
		{
			delivers++;
		}
	}
	CHECK(delivers == 1);
	CHECK(pthread_join(th, NULL) == 0);
	CHECK(atomic_load(&m.resumed) == 1);
}

int main(void)
{
	test_park_then_deliver();
	test_notify_before_park();
	test_merge();
	test_cross_thread();

	if (failures != 0)
	{
		fprintf(stderr, "parker: %d failure(s)\n", failures);
		return 1;
	}
	printf("parker: OK\n");
	return 0;
}
