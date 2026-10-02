#include "xr/xr_worker.h"

#include "xr/xr_env.h"
#include "xr/xr_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define XR_REG_BUCKETS 1024u

static _Thread_local xr_worker_t *tls_worker; /* V1:识别同线程 deliver */

struct xr_worker
{
	int epoll_fd;
	int event_fd;
	int cpu;
	int started;
	pthread_t thread;
	pthread_mutex_t lock; /* registry + ready 队列(V0:单锁,对齐 PEL sched->lock) */
	xr_task_t *ready_head;
	xr_task_t *ready_tail;
	xr_task_t *reg[XR_REG_BUCKETS];
	uint64_t next_id;
	_Atomic unsigned flags;        /* XR_WORKER_DIRECT / XR_WORKER_GATE */
	_Atomic uint32_t wake_pending; /* libuv async-pending 式合并 */
	_Atomic uint32_t sleeping;     /* V2:worker 正在(或即将)epoll_wait */
	_Atomic uint32_t stop;
	_Atomic uint64_t stat_wake_writes;
	_Atomic uint64_t stat_delivers;
	_Atomic uint64_t stat_runs;
	_Atomic uint64_t stat_wake_direct;
	_Atomic uint64_t stat_wake_gated;
};

/* ---------- ready 队列(调用方持 lock) ---------- */

static void push_ready_locked(xr_worker_t *w, xr_task_t *t)
{
	t->ready_next = NULL;
	if (w->ready_tail != NULL)
	{
		w->ready_tail->ready_next = t;
	}
	else
	{
		w->ready_head = t;
	}
	w->ready_tail = t;
}

static xr_task_t *pop_ready_locked(xr_worker_t *w)
{
	xr_task_t *t = w->ready_head;

	if (t == NULL)
	{
		return NULL;
	}
	w->ready_head = t->ready_next;
	if (w->ready_head == NULL)
	{
		w->ready_tail = NULL;
	}
	t->ready_next = NULL;
	return t;
}

/* ---------- registry(弱句柄 id → task,调用方持 lock) ---------- */

static void reg_insert_locked(xr_worker_t *w, xr_task_t *t)
{
	uint32_t b = (uint32_t)(t->id & (XR_REG_BUCKETS - 1u));
	t->reg_next = w->reg[b];
	w->reg[b] = t;
}

static xr_task_t *reg_find_locked(xr_worker_t *w, uint64_t id)
{
	uint32_t b = (uint32_t)(id & (XR_REG_BUCKETS - 1u));

	for (xr_task_t *t = w->reg[b]; t != NULL; t = t->reg_next)
	{
		if (t->id == id)
		{
			return t;
		}
	}
	return NULL;
}

static void reg_remove_locked(xr_worker_t *w, xr_task_t *t)
{
	uint32_t b = (uint32_t)(t->id & (XR_REG_BUCKETS - 1u));
	xr_task_t **pp = &w->reg[b];

	while (*pp != NULL)
	{
		if (*pp == t)
		{
			*pp = t->reg_next;
			return;
		}
		pp = &(*pp)->reg_next;
	}
}

/* ---------- 事件面(PEL:uv_async_send 恒发,libuv 侧 pending 合并) ---------- */

static void worker_wake(xr_worker_t *w)
{
	uint32_t expect = 0;

	if (atomic_compare_exchange_strong_explicit(&w->wake_pending, &expect, 1,
						    memory_order_acq_rel,
						    memory_order_relaxed))
	{
		uint64_t one = 1;
		ssize_t n = write(w->event_fd, &one, sizeof(one));
		(void)n;
		atomic_fetch_add_explicit(&w->stat_wake_writes, 1,
					  memory_order_relaxed);
	}
}

/* deliver 在 unpark 调用线程、持 parker pub 时执行(V0 形态) */
static void task_deliver(xr_parker_t *p, uint64_t payload, void *ctx)
{
	xr_waker_t *wk = ctx;
	xr_worker_t *w = wk->worker;
	unsigned flags = atomic_load_explicit(&w->flags, memory_order_relaxed);
	int pushed = 0;

	(void)p;
	(void)payload;

	pthread_mutex_lock(&w->lock);
	{
		xr_task_t *t = reg_find_locked(w, wk->id);
		if (t != NULL)
		{
			push_ready_locked(w, t);
			pushed = 1;
		}
	}
	pthread_mutex_unlock(&w->lock);

	if (pushed == 0)
	{
		return;
	}
	atomic_fetch_add_explicit(&w->stat_delivers, 1, memory_order_relaxed);

	/* V1:同线程安全点直投——worker 处理完当前 step 会再 drain 队列 */
	if ((flags & XR_WORKER_DIRECT) != 0 && tls_worker == w)
	{
		atomic_fetch_add_explicit(&w->stat_wake_direct, 1,
					  memory_order_relaxed);
		return;
	}
	/* V2:sleeping 门控——worker 未挂起时只入队,由 drain 兜底 */
	if ((flags & XR_WORKER_GATE) != 0 &&
	    atomic_load_explicit(&w->sleeping, memory_order_acquire) == 0)
	{
		atomic_fetch_add_explicit(&w->stat_wake_gated, 1,
					  memory_order_relaxed);
		return;
	}
	worker_wake(w);
}

static xr_task_t *worker_pop(xr_worker_t *w)
{
	xr_task_t *t;

	pthread_mutex_lock(&w->lock);
	t = pop_ready_locked(w);
	pthread_mutex_unlock(&w->lock);
	return t;
}

static void worker_run_task(xr_worker_t *w, xr_task_t *t)
{
	int rc;

	atomic_fetch_add_explicit(&w->stat_runs, 1, memory_order_relaxed);
	rc = t->fn(t);
	if (rc == XR_TASK_RUN_AGAIN)
	{
		pthread_mutex_lock(&w->lock);
		push_ready_locked(w, t);
		pthread_mutex_unlock(&w->lock);
	}
	else if (rc == XR_TASK_DONE)
	{
		pthread_mutex_lock(&w->lock);
		reg_remove_locked(w, t);
		pthread_mutex_unlock(&w->lock);
	}
}

static void *worker_main(void *arg)
{
	xr_worker_t *w = arg;

	tls_worker = w;
	if (w->cpu >= 0)
	{
		int rc = xr_pin_to_cpu(w->cpu);
		if (rc != 0)
		{
			XR_LOGW("worker pin cpu%d failed: %d", w->cpu, rc);
		}
	}

	for (;;)
	{
		xr_task_t *t = worker_pop(w);

		if (t != NULL)
		{
			worker_run_task(w, t);
			continue;
		}

		if (atomic_load_explicit(&w->stop, memory_order_acquire) != 0)
		{
			break;
		}

		/* V2:先置 sleeping 再复核队列,关闭
		 * "pop 空 → 生产者入队且看到 sleeping=0 → 丢唤醒" 窗口 */
		atomic_store_explicit(&w->sleeping, 1, memory_order_release);
		t = worker_pop(w);
		if (t != NULL)
		{
			atomic_store_explicit(&w->sleeping, 0,
					      memory_order_release);
			worker_run_task(w, t);
			continue;
		}

		{
			struct epoll_event ev;
			int n = epoll_wait(w->epoll_fd, &ev, 1, -1);

			atomic_store_explicit(&w->sleeping, 0,
					      memory_order_release);
			if (n > 0)
			{
				uint64_t v;
				ssize_t r = read(w->event_fd, &v, sizeof(v));
				(void)r;
				/* 先清 pending 再回到循环 drain:期间入队的任务
				 * 看到 pending=0 会再写 eventfd,不丢唤醒 */
				atomic_store_explicit(&w->wake_pending, 0,
						      memory_order_release);
			}
		}
	}
	return NULL;
}

xr_worker_t *xr_worker_create(int cpu)
{
	xr_worker_t *w = calloc(1, sizeof(*w));

	if (w == NULL)
	{
		return NULL;
	}
	w->epoll_fd = -1;
	w->event_fd = -1;
	w->cpu = cpu;
	w->next_id = 1;

	if (pthread_mutex_init(&w->lock, NULL) != 0)
	{
		free(w);
		return NULL;
	}
	w->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (w->epoll_fd < 0)
	{
		goto fail;
	}
	w->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (w->event_fd < 0)
	{
		goto fail;
	}
	{
		struct epoll_event ev;

		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.u64 = 1;
		if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, w->event_fd, &ev) != 0)
		{
			goto fail;
		}
	}
	if (pthread_create(&w->thread, NULL, worker_main, w) != 0)
	{
		goto fail;
	}
	w->started = 1;
	return w;

fail:
	XR_LOGE("worker create failed: %s", strerror(errno));
	if (w->event_fd >= 0)
	{
		close(w->event_fd);
	}
	if (w->epoll_fd >= 0)
	{
		close(w->epoll_fd);
	}
	pthread_mutex_destroy(&w->lock);
	free(w);
	return NULL;
}

void xr_worker_destroy(xr_worker_t *w)
{
	if (w == NULL)
	{
		return;
	}
	atomic_store_explicit(&w->stop, 1, memory_order_release);
	worker_wake(w);
	if (w->started != 0)
	{
		pthread_join(w->thread, NULL);
	}
	close(w->event_fd);
	close(w->epoll_fd);
	pthread_mutex_destroy(&w->lock);
	free(w);
}

void xr_worker_set_flags(xr_worker_t *w, unsigned flags)
{
	atomic_store_explicit(&w->flags, flags, memory_order_relaxed);
}

void xr_worker_stats(xr_worker_t *w, xr_worker_stats_t *out)
{
	out->wake_writes = atomic_load_explicit(&w->stat_wake_writes,
						memory_order_relaxed);
	out->delivers = atomic_load_explicit(&w->stat_delivers,
					     memory_order_relaxed);
	out->runs = atomic_load_explicit(&w->stat_runs, memory_order_relaxed);
	out->wake_direct = atomic_load_explicit(&w->stat_wake_direct,
						memory_order_relaxed);
	out->wake_gated = atomic_load_explicit(&w->stat_wake_gated,
					       memory_order_relaxed);
}

/* ---------- task ---------- */

void xr_task_init(xr_task_t *t, xr_task_fn fn, void *user)
{
	memset(t, 0, sizeof(*t));
	t->fn = fn;
	t->user = user;
}

xr_parker_t *xr_task_parker(xr_task_t *t)
{
	return &t->parker;
}

void xr_task_spawn(xr_worker_t *w, xr_task_t *t)
{
	pthread_mutex_lock(&w->lock);
	t->owner = w;
	t->id = w->next_id;
	w->next_id++;
	t->waker.worker = w;
	t->waker.id = t->id;
	xr_parker_init(&t->parker, task_deliver, &t->waker);
	reg_insert_locked(w, t);
	push_ready_locked(w, t);
	pthread_mutex_unlock(&w->lock);
	worker_wake(w);
}
