#include "xr/xr_worker.h"

#include "xr/xr_env.h"
#include "xr/xr_log.h"
#include "xr/xr_mpsc.h"

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
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
	xr_task_t *ready_head; /* V0 主队列;V3=溢出链 */
	xr_task_t *ready_tail;
	xr_mpsc_t *mpsc;       /* V3:lock-free 主队列 */
	xr_task_t *reg[XR_REG_BUCKETS];
	uint64_t next_id;
	_Atomic unsigned flags;        /* XR_WORKER_DIRECT / GATE / MPSC / WAKER_DIRECT */
	_Atomic uint32_t wake_pending; /* libuv async-pending 式合并 */
	_Atomic uint32_t sleeping;     /* V2:worker 正在(或即将)epoll_wait */
	_Atomic uint32_t fut_word;     /* V5:futex 睡眠字(0=可睡,1=已唤醒) */
	_Atomic uint32_t stop;
	_Atomic uint64_t stat_wake_writes;
	_Atomic uint64_t stat_delivers;
	_Atomic uint64_t stat_runs;
	_Atomic uint64_t stat_wake_direct;
	_Atomic uint64_t stat_wake_gated;
	_Atomic uint64_t stat_wake_futex;
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

/* 入队唯一漏斗:V3 先走 MPSC 环(满/未启用→ready 链表=溢出链,持 lock)。 */
static void worker_enqueue(xr_worker_t *w, xr_task_t *t)
{
	unsigned flags = atomic_load_explicit(&w->flags, memory_order_relaxed);

	if ((flags & XR_WORKER_MPSC) != 0 && xr_mpsc_try_push(w->mpsc, t))
	{
		return;
	}
	pthread_mutex_lock(&w->lock);
	push_ready_locked(w, t);
	pthread_mutex_unlock(&w->lock);
}

/* 出队唯一漏斗:V3 先 MPSC 环,再溢出链。 */
static xr_task_t *worker_dequeue(xr_worker_t *w)
{
	unsigned flags = atomic_load_explicit(&w->flags, memory_order_relaxed);
	xr_task_t *t = NULL;

	if ((flags & XR_WORKER_MPSC) != 0)
	{
		t = xr_mpsc_pop(w->mpsc);
		if (t != NULL)
		{
			return t;
		}
	}
	pthread_mutex_lock(&w->lock);
	t = pop_ready_locked(w);
	pthread_mutex_unlock(&w->lock);
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

/* V5:futex 睡眠字置位 + 唤醒。worker 睡眠前清零(先于 sleeping=1),
 * 生产者见 sleeping=1 后置位;futex_wait 以字值做原子复核,不丢唤醒。
 * 0→1 才发 futex_wake(latch 合并):与 eventfd pending 同型,避免
 * 饱和形态下每次 deliver 一个 syscall。 */
static void worker_futex_wake(xr_worker_t *w)
{
	if (atomic_exchange_explicit(&w->fut_word, 1, memory_order_acq_rel) == 0)
	{
		(void)syscall(SYS_futex, &w->fut_word, FUTEX_WAKE_PRIVATE, 1,
			      NULL, NULL, 0);
		atomic_fetch_add_explicit(&w->stat_wake_futex, 1,
					  memory_order_relaxed);
	}
}

static void worker_eventfd_wake(xr_worker_t *w)
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

static void worker_wake(xr_worker_t *w)
{
	unsigned flags = atomic_load_explicit(&w->flags, memory_order_relaxed);

	if ((flags & XR_WORKER_FUTEX) != 0)
	{
		worker_futex_wake(w);
		return;
	}
	worker_eventfd_wake(w);
}

/* deliver 唯一漏斗:解析/直接指针 + 入队 + transport(调用方持 pub)。
 * dt!=NULL = V4a 直接 waker;否则经 wk 弱句柄解析(miss 即丢)。
 * 非 MPSC:find+push 必须同锁原子(防 DONE 摘除后重新入队);
 * MPSC:解析(锁内)与入队(无锁环)分离,由"至多一个 ready 条目"不变式保证。 */
static void deliver_impl(xr_worker_t *w, const xr_waker_t *wk, xr_task_t *dt)
{
	unsigned flags = atomic_load_explicit(&w->flags, memory_order_relaxed);
	xr_task_t *t = NULL;

	if ((flags & XR_WORKER_MPSC) != 0)
	{
		if (dt != NULL)
		{
			t = dt;
		}
		else if (wk != NULL)
		{
			pthread_mutex_lock(&w->lock);
			t = reg_find_locked(w, wk->id);
			pthread_mutex_unlock(&w->lock);
		}
		if (t == NULL)
		{
			return;
		}
		worker_enqueue(w, t);
	}
	else
	{
		pthread_mutex_lock(&w->lock);
		if (dt != NULL)
		{
			t = dt;
		}
		else if (wk != NULL)
		{
			t = reg_find_locked(w, wk->id);
		}
		if (t != NULL)
		{
			push_ready_locked(w, t);
		}
		pthread_mutex_unlock(&w->lock);
		if (t == NULL)
		{
			return;
		}
	}
	atomic_fetch_add_explicit(&w->stat_delivers, 1, memory_order_relaxed);

	/* V1:同线程安全点直投——worker 处理完当前 step 会再 drain 队列 */
	if ((flags & XR_WORKER_DIRECT) != 0 && tls_worker == w)
	{
		atomic_fetch_add_explicit(&w->stat_wake_direct, 1,
					  memory_order_relaxed);
		return;
	}
	/* V2:sleeping 门控——依赖 mutex 队列对"入队/复核"的串行化;
	 * MPSC 无锁队列破坏该协议,FUTEX 有自带单字门控(sleeping 不再维护),
	 * 故二者下 GATE 自动失效(见 xr_worker.h 契约)。 */
	if ((flags & XR_WORKER_GATE) != 0 &&
	    (flags & (XR_WORKER_MPSC | XR_WORKER_FUTEX)) == 0 &&
	    atomic_load_explicit(&w->sleeping, memory_order_acquire) == 0)
	{
		atomic_fetch_add_explicit(&w->stat_wake_gated, 1,
					  memory_order_relaxed);
		return;
	}
	/* V5:futex transport(单字协议)——0→1 才发 futex_wake;worker awake 时
	 * (fut_word=1)零 syscall。不依赖队列串行化,MPSC|FUTEX 亦正确。 */
	if ((flags & XR_WORKER_FUTEX) != 0)
	{
		worker_futex_wake(w);
		return;
	}
	worker_wake(w);
}

/* V0 形态:经弱句柄 registry 解析 */
static void task_deliver(xr_parker_t *p, uint64_t payload, void *ctx)
{
	xr_waker_t *wk = ctx;

	(void)p;
	(void)payload;
	deliver_impl(wk->worker, wk, NULL);
}

/* V4a:ctx 直接为 task 指针,零查找 */
static void task_deliver_direct(xr_parker_t *p, uint64_t payload, void *ctx)
{
	xr_task_t *t = ctx;

	(void)p;
	(void)payload;
	deliver_impl(t->owner, NULL, t);
}

static xr_task_t *worker_pop(xr_worker_t *w)
{
	return worker_dequeue(w);
}

static void worker_run_task(xr_worker_t *w, xr_task_t *t)
{
	int rc;

	atomic_fetch_add_explicit(&w->stat_runs, 1, memory_order_relaxed);
	rc = t->fn(t);
	if (rc == XR_TASK_RUN_AGAIN)
	{
		worker_enqueue(w, t);
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

		/* V5 单字 futex 协议(与 eventfd/sleeping 协议并列):
		 * 0=声明睡眠(仅此刻生产者会 futex_wake),1=awake/有唤醒在途。
		 * 先置 0 再复核队列,不依赖 mutex 串行化。 */
		if ((atomic_load_explicit(&w->flags, memory_order_relaxed) &
		     XR_WORKER_FUTEX) != 0)
		{
			atomic_store_explicit(&w->fut_word, 0,
					      memory_order_release);
			t = worker_pop(w);
			if (t != NULL)
			{
				atomic_store_explicit(&w->fut_word, 1,
						      memory_order_release);
				worker_run_task(w, t);
				continue;
			}
			{
				struct timespec ts;

				ts.tv_sec = 0;
				ts.tv_nsec = 100000000; /* 100ms:仅为观察 stop */
				(void)syscall(SYS_futex, &w->fut_word,
					      FUTEX_WAIT_PRIVATE, 0, &ts, NULL,
					      0);
				atomic_store_explicit(&w->fut_word, 1,
						      memory_order_release);
			}
			continue;
		}

		/* V2:先置 sleeping 再复核队列,关闭
		 * "pop 空 → 生产者入队且看到 sleeping=0 → 丢唤醒" 窗口。 */
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
	/* V5:初始 awake(=1),避免首次唤醒的无谓 futex_wake */
	atomic_store_explicit(&w->fut_word, 1, memory_order_relaxed);

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
	w->mpsc = xr_mpsc_create(4096);
	if (w->mpsc == NULL)
	{
		goto fail;
	}
	if (pthread_create(&w->thread, NULL, worker_main, w) != 0)
	{
		goto fail;
	}
	w->started = 1;
	return w;

fail:
	XR_LOGE("worker create failed: %s", strerror(errno));
	xr_mpsc_destroy(w->mpsc);
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
	xr_mpsc_destroy(w->mpsc);
	close(w->event_fd);
	close(w->epoll_fd);
	pthread_mutex_destroy(&w->lock);
	free(w);
}

void xr_worker_set_flags(xr_worker_t *w, unsigned flags)
{
	atomic_store_explicit(&w->flags, flags, memory_order_relaxed);
	if ((flags & XR_WORKER_FUTEX) != 0)
	{
		/* 配置期(一次性):worker 线程可能已按旧 flags 睡在 epoll_wait,
		 * 而新 transport 走 futex_wake——双写覆盖两种睡眠原语,使其
		 * 回循环重读 flags。热路径无此开销。 */
		worker_eventfd_wake(w);
		worker_futex_wake(w);
	}
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
	out->wake_futex = atomic_load_explicit(&w->stat_wake_futex,
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
	unsigned flags = atomic_load_explicit(&w->flags, memory_order_relaxed);
	int waker_direct = (flags & XR_WORKER_WAKER_DIRECT) != 0;

	pthread_mutex_lock(&w->lock);
	t->owner = w;
	t->id = w->next_id;
	w->next_id++;
	t->waker.worker = w;
	t->waker.id = t->id;
	if (waker_direct)
	{
		/* V4a:ctx=task 指针,跳过 registry(生命期前置见 xr_task.h) */
		xr_parker_init(&t->parker, task_deliver_direct, t);
	}
	else
	{
		xr_parker_init(&t->parker, task_deliver, &t->waker);
		reg_insert_locked(w, t);
	}
	pthread_mutex_unlock(&w->lock);
	worker_enqueue(w, t); /* L2:注册先于入队(MPSC 模式锁外入环) */
	worker_wake(w);
}
