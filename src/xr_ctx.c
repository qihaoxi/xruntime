#define _GNU_SOURCE
#include "xr/xr_ctx.h"

#include "xr/xr_log.h"
#include "xr/xr_task.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * stackful L0 后端:xr_cpu_switch + 栈分配(U18)。
 * - MMAP:每 ctx mmap(guard 低页 + RW 栈),2 VMA/个;
 * - ARENA:进程级 arena bump,无 guard;相邻匿名映射被内核合并(VMA≈O(1)),
 *   创建 ~ns;溢出无保护;
 * - ARENA_GUARD:arena + UFFD-WP 保护页(不拆 VMA),溢出写触发 fault,
 *   内部 handler 计数并解除保护(写入重试成功)。UFFD 不可用自动降级 ARENA。
 * 首次 resume 进入伪造帧 → xr_ctx_trampoline → xr_ctx_entry_c。
 */

#define XR_CTX_MIN_STACK (16u * 1024u)
#define XR_CTX_DEFAULT_STACK (64u * 1024u)
#define XR_ARENA_DEFAULT ((size_t)4 << 30) /* 4GB VA,NORESERVE,按需增长 */

struct xr_ctx
{
	xr_task_t *task;
	xr_ctx_entry_fn entry;
	_Atomic int state;
	xr_ctx_alloc_t alloc;
	char *block;     /* MMAP:mmap 基址;ARENA:arena 块基址(含 guard) */
	size_t block_size;
	char *guard;     /* ARENA_GUARD:保护页地址,否则 NULL */
	char *stack_top; /* 16 对齐 */
	void *sp;        /* 任务栈保存点 */
	void *worker_sp; /* 进入前的 worker 栈保存点 */
};

/* ---------- 进程级 arena 分配器 ---------- */

typedef struct xr_arena
{
	char *base;
	size_t size;
	size_t used;
	struct xr_arena *next;
} xr_arena_t;

static xr_ctx_alloc_t g_alloc = XR_CTX_ALLOC_MMAP;
static size_t g_arena_bytes;
static xr_arena_t *g_arenas;
static void *g_free_list; /* 块首字=next,次字=size */
static pthread_mutex_t g_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_page;

/* UFFD-WP(guard 模式) */
static int g_uffd = -1;
static int g_uffd_tried;
static pthread_t g_uffd_thread;
static _Atomic uint64_t g_uffd_faults;
static _Atomic uintptr_t g_uffd_last;

static void *uffd_handler(void *arg)
{
	struct pollfd pfd;

	(void)arg;
	pfd.fd = g_uffd;
	pfd.events = POLLIN;
	for (;;)
	{
		int pr = poll(&pfd, 1, -1);

		if (pr <= 0)
		{
			continue;
		}
		for (;;)
		{
			struct uffd_msg msg;
			ssize_t n = read(g_uffd, &msg, sizeof(msg));

			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					break;
				}
				if (errno == EINTR)
				{
					continue;
				}
				return NULL;
			}
			if (n != (ssize_t)sizeof(msg))
			{
				break;
			}
			if (msg.event == UFFD_EVENT_PAGEFAULT &&
			    (msg.arg.pagefault.flags &
			     UFFD_PAGEFAULT_FLAG_WP) != 0)
			{
				unsigned long page =
					(unsigned long)msg.arg.pagefault.address &
					~(unsigned long)(g_page - 1);
				struct uffdio_writeprotect wp;

				atomic_fetch_add_explicit(&g_uffd_faults, 1,
							  memory_order_relaxed);
				atomic_store_explicit(&g_uffd_last,
						      (uintptr_t)msg.arg.pagefault
							      .address,
						      memory_order_relaxed);
				/* 解除保护使写入重试成功(测试语义;生产应上报/终止) */
				memset(&wp, 0, sizeof(wp));
				wp.range.start = page;
				wp.range.len = (unsigned long)g_page;
				wp.mode = 0;
				(void)ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp);
			}
		}
	}
}

static int uffd_init_locked(void)
{
	struct uffdio_api api;
	int fd;

	if (g_uffd_tried)
	{
		return g_uffd >= 0 ? 0 : -1;
	}
	g_uffd_tried = 1;
	fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
	{
		XR_LOGW("uffd unavailable (%s), arena guard 降级为无 guard",
			strerror(errno));
		return -1;
	}
	memset(&api, 0, sizeof(api));
	api.api = UFFD_API;
	api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
	if (ioctl(fd, UFFDIO_API, &api) < 0 ||
	    (api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP) == 0)
	{
		XR_LOGW("uffd WP feature unavailable, 降级为无 guard");
		close(fd);
		return -1;
	}
	g_uffd = fd;
	if (pthread_create(&g_uffd_thread, NULL, uffd_handler, NULL) != 0)
	{
		close(fd);
		g_uffd = -1;
		return -1;
	}
	return 0;
}

static void uffd_register_locked(void *base, size_t size)
{
	struct uffdio_register ur;

	memset(&ur, 0, sizeof(ur));
	ur.range.start = (unsigned long)base;
	ur.range.len = size;
	ur.mode = UFFDIO_REGISTER_MODE_WP;
	if (ioctl(g_uffd, UFFDIO_REGISTER, &ur) < 0)
	{
		XR_LOGW("uffd register arena failed: %s", strerror(errno));
	}
}

static void uffd_wp_guard(void *page)
{
	struct uffdio_writeprotect wp;

	/* 先落页:WP 对未 present 的页不产生 WP 事件(实测) */
	*(volatile char *)page = 0;
	memset(&wp, 0, sizeof(wp));
	wp.range.start = (unsigned long)page;
	wp.range.len = (unsigned long)g_page;
	wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
	if (ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp) < 0)
	{
		XR_LOGW("uffd writeprotect failed: %s", strerror(errno));
	}
}

static void *arena_alloc(size_t need)
{
	void **pp;
	xr_arena_t *a;

	pthread_mutex_lock(&g_alloc_lock);
	/* 空闲链优先(同尺寸复用) */
	for (pp = &g_free_list; *pp != NULL; pp = (void **)*pp)
	{
		char *b = *pp;
		size_t bs = *(size_t *)(b + sizeof(void *));

		if (bs >= need)
		{
			*pp = *(void **)b;
			pthread_mutex_unlock(&g_alloc_lock);
			return b;
		}
	}
	a = g_arenas;
	if (a == NULL || a->used + need > a->size)
	{
		size_t asz = g_arena_bytes != 0 ? g_arena_bytes
						: XR_ARENA_DEFAULT;
		char *base;

		if (asz < need)
		{
			asz = need;
		}
		base = mmap(NULL, asz, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (base == MAP_FAILED)
		{
			pthread_mutex_unlock(&g_alloc_lock);
			return NULL;
		}
		if (g_alloc == XR_CTX_ALLOC_ARENA_GUARD && g_uffd >= 0)
		{
			uffd_register_locked(base, asz);
		}
		a = calloc(1, sizeof(*a));
		if (a == NULL)
		{
			munmap(base, asz);
			pthread_mutex_unlock(&g_alloc_lock);
			return NULL;
		}
		a->base = base;
		a->size = asz;
		a->next = g_arenas;
		g_arenas = a;
	}
	{
		char *p = a->base + a->used;

		a->used += need;
		pthread_mutex_unlock(&g_alloc_lock);
		return p;
	}
}

static void arena_free(void *block, size_t size)
{
	pthread_mutex_lock(&g_alloc_lock);
	*(void **)block = g_free_list;
	*(size_t *)((char *)block + sizeof(void *)) = size;
	g_free_list = block;
	pthread_mutex_unlock(&g_alloc_lock);
}

void xr_ctx_set_alloc(xr_ctx_alloc_t mode, size_t arena_bytes)
{
	pthread_mutex_lock(&g_alloc_lock);
	g_alloc = mode;
	g_arena_bytes = arena_bytes;
	if (mode == XR_CTX_ALLOC_ARENA_GUARD)
	{
		(void)uffd_init_locked();
	}
	pthread_mutex_unlock(&g_alloc_lock);
}

void *xr_ctx_guard_page(xr_ctx_t *c)
{
	return c != NULL ? c->guard : NULL;
}

uint64_t xr_ctx_uffd_faults(void)
{
	return atomic_load_explicit(&g_uffd_faults, memory_order_relaxed);
}

/* ---------- ctx ---------- */

/* asm 着陆点调用(符号在 xr_switch_x86_64.S 引用);原型抑制
 * -Wmissing-prototypes。entry 返回即 DONE,切回 worker 且不再恢复。 */
void xr_ctx_trampoline(void);
void xr_ctx_entry_c(xr_ctx_t *c);
void xr_ctx_entry_c(xr_ctx_t *c)
{
	c->entry(c->task);
	atomic_store_explicit(&c->state, XR_CTX_DONE, memory_order_release);
	xr_cpu_switch(&c->sp, c->worker_sp);
	abort(); /* DONE 后不得再 resume */
}

static size_t round_up(size_t n, size_t align)
{
	return (n + align - 1u) & ~(align - 1u);
}

static void ctx_arm(xr_ctx_t *c)
{
	void **frame = (void **)(c->stack_top - 7 * sizeof(void *));

	/* 伪造帧:6 callee-saved + ret 地址(trampoline),r12=ctx。
	 * pop6+ret 后 rsp=stack_top(16 对齐),满足 call 前对齐要求。 */
	frame[0] = NULL; /* r15 */
	frame[1] = NULL; /* r14 */
	frame[2] = NULL; /* r13 */
	frame[3] = c;    /* r12 → trampoline 取出为 rdi */
	frame[4] = NULL; /* rbx */
	frame[5] = NULL; /* rbp */
	frame[6] = (void *)xr_ctx_trampoline;
	c->sp = frame;
	atomic_store_explicit(&c->state, XR_CTX_RUNNABLE, memory_order_relaxed);
}

xr_ctx_t *xr_ctx_create(xr_task_t *t, xr_ctx_entry_fn entry, size_t stack_size)
{
	xr_ctx_t *c;
	long page = sysconf(_SC_PAGESIZE);
	size_t sp_size;
	xr_ctx_alloc_t mode;

	if (page <= 0)
	{
		return NULL;
	}
	if (g_page == 0)
	{
		g_page = page;
	}
	if (stack_size == 0)
	{
		stack_size = XR_CTX_DEFAULT_STACK;
	}
	if (stack_size < XR_CTX_MIN_STACK)
	{
		stack_size = XR_CTX_MIN_STACK;
	}
	sp_size = round_up(stack_size, (size_t)page);

	c = calloc(1, sizeof(*c));
	if (c == NULL)
	{
		return NULL;
	}
	mode = g_alloc;
	c->alloc = mode;
	if (mode == XR_CTX_ALLOC_MMAP)
	{
		c->block_size = (size_t)page + sp_size;
		c->block = mmap(NULL, c->block_size, PROT_NONE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (c->block == MAP_FAILED)
		{
			free(c);
			return NULL;
		}
		/* 低页 guard;高 sp_size 为可读写栈(向下增长,栈顶在高地址) */
		if (mprotect(c->block + page, sp_size,
			     PROT_READ | PROT_WRITE) != 0)
		{
			munmap(c->block, c->block_size);
			free(c);
			return NULL;
		}
		c->stack_top = c->block + page + sp_size;
	}
	else
	{
		int want_guard = (mode == XR_CTX_ALLOC_ARENA_GUARD);

		c->block_size = (want_guard ? (size_t)page : 0) + sp_size;
		c->block = arena_alloc(c->block_size);
		if (c->block == NULL)
		{
			free(c);
			return NULL;
		}
		if (want_guard)
		{
			c->guard = c->block;
			if (g_uffd >= 0)
			{
				uffd_wp_guard(c->guard);
			}
			else
			{
				c->guard = NULL; /* 降级:无 guard(块仍含预留页) */
			}
		}
		c->stack_top = c->block +
			       (want_guard ? (size_t)page : 0) + sp_size;
	}
	c->task = t;
	c->entry = entry;
	ctx_arm(c);
	return c;
}

void xr_ctx_destroy(xr_ctx_t *c)
{
	if (c == NULL)
	{
		return;
	}
	if (c->alloc == XR_CTX_ALLOC_MMAP)
	{
		munmap(c->block, c->block_size);
	}
	else
	{
		arena_free(c->block, c->block_size);
	}
	free(c);
}

void xr_ctx_resume(xr_ctx_t *c)
{
	if (atomic_load_explicit(&c->state, memory_order_acquire) == XR_CTX_DONE)
	{
		return;
	}
	atomic_store_explicit(&c->state, XR_CTX_RUNNABLE, memory_order_release);
	xr_cpu_switch(&c->worker_sp, c->sp);
}

void xr_ctx_suspend(xr_ctx_t *c)
{
	atomic_store_explicit(&c->state, XR_CTX_PARKED, memory_order_release);
	xr_cpu_switch(&c->sp, c->worker_sp);
	/* 恢复后返回调用方;state 已由 resume 置 RUNNABLE */
}

xr_ctx_state_t xr_ctx_state(const xr_ctx_t *c)
{
	return (xr_ctx_state_t)atomic_load_explicit(&c->state,
						    memory_order_acquire);
}
