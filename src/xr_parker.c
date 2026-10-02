#include "xr/xr_parker.h"

#include "xr/xr_time.h"

#include <assert.h>
#include <stddef.h>

void xr_parker_init(xr_parker_t *p, xr_parker_deliver_fn fn, void *ctx)
{
	atomic_store_explicit(&p->state, XR_PARK_IDLE, memory_order_relaxed);
	atomic_store_explicit(&p->pub, 0, memory_order_relaxed);
	atomic_store_explicit(&p->payload, 0, memory_order_relaxed);
	p->deliver = fn;
	p->ctx = ctx;
}

xr_park_result_t xr_parker_park(xr_parker_t *p)
{
	for (;;)
	{
		uint32_t st = atomic_load_explicit(&p->state, memory_order_acquire);

		if (st == XR_PARK_NOTIFIED)
		{
			uint32_t expect = XR_PARK_NOTIFIED;
			if (atomic_compare_exchange_weak_explicit(
				    &p->state, &expect, XR_PARK_IDLE,
				    memory_order_acq_rel, memory_order_acquire))
			{
				return XR_PARK_CONSUMED;
			}
			continue;
		}
		if (st == XR_PARK_IDLE)
		{
			uint32_t expect = XR_PARK_IDLE;
			if (atomic_compare_exchange_weak_explicit(
				    &p->state, &expect, XR_PARK_PARKED,
				    memory_order_acq_rel, memory_order_acquire))
			{
				return XR_PARK_SUSPENDED;
			}
			continue;
		}

		/* PARKED:单等待者协议,双 park 即误用 */
		assert(0 && "xr_parker: double park");
		return XR_PARK_SUSPENDED;
	}
}

xr_unpark_result_t xr_parker_unpark(xr_parker_t *p, uint64_t payload)
{
	xr_unpark_result_t r;
	uint32_t expect = 0;

	while (!atomic_compare_exchange_weak_explicit(
		&p->pub, &expect, 1, memory_order_acquire, memory_order_relaxed))
	{
		expect = 0;
		xr_cpu_relax();
	}

	atomic_store_explicit(&p->payload, payload, memory_order_relaxed);

	for (;;)
	{
		uint32_t st = atomic_load_explicit(&p->state, memory_order_acquire);

		if (st == XR_PARK_PARKED)
		{
			uint32_t e = st;
			if (atomic_compare_exchange_weak_explicit(
				    &p->state, &e, XR_PARK_NOTIFIED,
				    memory_order_acq_rel, memory_order_relaxed))
			{
				r = XR_UNPARK_DELIVER;
				if (p->deliver != NULL)
				{
					p->deliver(p, payload, p->ctx);
				}
				break;
			}
			continue;
		}
		if (st == XR_PARK_IDLE)
		{
			uint32_t e = st;
			if (atomic_compare_exchange_weak_explicit(
				    &p->state, &e, XR_PARK_NOTIFIED,
				    memory_order_acq_rel, memory_order_relaxed))
			{
				r = XR_UNPARK_STORED;
				break;
			}
			continue;
		}
		r = XR_UNPARK_MERGED;
		break;
	}

	atomic_store_explicit(&p->pub, 0, memory_order_release);
	return r;
}

xr_parker_state_t xr_parker_state(const xr_parker_t *p)
{
	return (xr_parker_state_t)atomic_load_explicit(&p->state,
						       memory_order_acquire);
}
