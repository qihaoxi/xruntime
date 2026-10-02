#include "xr/xr_mpsc.h"

#include "xr/xr_time.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	_Atomic uint64_t seq; /* 圈计数:初值=下标,pop 后=pos+capacity */
	xr_task_t *task;
} xr_mpsc_slot_t;

struct xr_mpsc
{
	uint32_t capacity;
	uint32_t mask;
	_Atomic uint64_t head; /* 仅消费者写 */
	_Atomic uint64_t tail; /* 多生产者 fetch_add */
	xr_mpsc_slot_t *slots;
};

xr_mpsc_t *xr_mpsc_create(uint32_t capacity)
{
	xr_mpsc_t *m;

	if (capacity == 0 || (capacity & (capacity - 1u)) != 0)
	{
		return NULL;
	}
	m = calloc(1, sizeof(*m));
	if (m == NULL)
	{
		return NULL;
	}
	m->slots = calloc(capacity, sizeof(*m->slots));
	if (m->slots == NULL)
	{
		free(m);
		return NULL;
	}
	m->capacity = capacity;
	m->mask = capacity - 1u;
	for (uint32_t i = 0; i < capacity; i++)
	{
		atomic_store_explicit(&m->slots[i].seq, (uint64_t)i,
				      memory_order_relaxed);
	}
	return m;
}

void xr_mpsc_destroy(xr_mpsc_t *m)
{
	if (m == NULL)
	{
		return;
	}
	free(m->slots);
	free(m);
}

bool xr_mpsc_try_push(xr_mpsc_t *m, xr_task_t *t)
{
	uint64_t tail;
	uint64_t head;
	uint64_t pos;
	xr_mpsc_slot_t *s;

	/* 预检满:直接交给溢出链(不 claim 后再放弃,避免留洞) */
	tail = atomic_load_explicit(&m->tail, memory_order_relaxed);
	head = atomic_load_explicit(&m->head, memory_order_acquire);
	if (tail - head >= (uint64_t)m->capacity)
	{
		return false;
	}

	pos = atomic_fetch_add_explicit(&m->tail, 1, memory_order_relaxed);
	s = &m->slots[pos & m->mask];
	/* 等待上一圈该位被消费者释放;消费者恒前进,等待有界 */
	while (atomic_load_explicit(&s->seq, memory_order_acquire) != pos)
	{
		xr_cpu_relax();
	}
	s->task = t;
	atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
	return true;
}

xr_task_t *xr_mpsc_pop(xr_mpsc_t *m)
{
	uint64_t pos = atomic_load_explicit(&m->head, memory_order_relaxed);
	xr_mpsc_slot_t *s = &m->slots[pos & m->mask];
	xr_task_t *t;

	if (atomic_load_explicit(&s->seq, memory_order_acquire) != pos + 1)
	{
		return NULL;
	}
	t = s->task;
	atomic_store_explicit(&s->seq, pos + (uint64_t)m->capacity,
			      memory_order_release);
	atomic_store_explicit(&m->head, pos + 1, memory_order_relaxed);
	return t;
}
