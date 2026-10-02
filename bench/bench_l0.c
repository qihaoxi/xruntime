#include "xr/xr_env.h"
#include "xr/xr_log.h"
#include "xr/xr_task.h"
#include "xr/xr_time.h"
#include "xr/xr_worker.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * L0 仪器标定与创建/销毁成本(02 §0.5.2 对照):
 *  --mode=calib : 两栈间 xr_cpu_switch 纯切换(2 次/迭代),报 ns/switch;
 *  --mode=create: --l0=stackless|fibre 的 spawn→完成→销毁,报 ns/个 + RSS。
 *   bench_l0 [--mode calib|create] [--l0 stackless|fibre] [--ops N]
 *            [--stack BYTES]
 */

static _Atomic int calib_done;
static void *calib_sp;
static void *main_sp;
static uint64_t calib_iters;

static void calib_target(void);
static void calib_target(void)
{
	for (uint64_t i = 0; i < calib_iters; i++)
	{
		xr_cpu_switch(&calib_sp, main_sp);
	}
	atomic_store_explicit(&calib_done, 1, memory_order_release);
	xr_cpu_switch(&calib_sp, main_sp);
	abort(); /* main 不再切回 */
}

static void print_rss(const char *tag)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	int n = 0;

	if (f == NULL)
	{
		return;
	}
	printf("  rss[%s]:", tag);
	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (strncmp(line, "VmPeak:", 7) == 0 ||
		    strncmp(line, "VmRSS:", 6) == 0)
		{
			line[strcspn(line, "\n")] = '\0';
			printf(" %s;", line);
			n++;
		}
	}
	printf("%s\n", n == 0 ? " (unavailable)" : "");
	(void)fclose(f);
}

static int run_calib(uint64_t iters)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t sp_size = 64u * 1024u;
	size_t map_size = (size_t)page + sp_size;
	char *map = mmap(NULL, map_size, PROT_NONE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	void **frame;
	uint64_t t0, t1;

	if (map == MAP_FAILED)
	{
		XR_LOGE("calib mmap failed");
		return 1;
	}
	if (mprotect(map + page, sp_size, PROT_READ | PROT_WRITE) != 0)
	{
		munmap(map, map_size);
		return 1;
	}
	calib_iters = iters;
	frame = (void **)(map + page + sp_size - 7 * sizeof(void *));
	for (int i = 0; i < 6; i++)
	{
		frame[i] = NULL;
	}
	frame[6] = (void *)calib_target;
	calib_sp = frame;
	atomic_store(&calib_done, 0);

	t0 = xr_tsc();
	xr_cpu_switch(&main_sp, calib_sp);
	while (atomic_load_explicit(&calib_done, memory_order_acquire) == 0)
	{
		xr_cpu_switch(&main_sp, calib_sp);
	}
	t1 = xr_tsc();

	printf("bench_l0 calib: iters=%" PRIu64 " switches=%" PRIu64
	       "  %.1f ns/switch\n",
	       iters, 2 * iters + 2,
	       (double)xr_tsc_to_ns(t1 - t0) / (double)(2 * iters + 2));
	munmap(map, map_size);
	return 0;
}

/* ---------- create:spawn→完成(含 ctx 创建/销毁) ---------- */

typedef struct
{
	xr_task_t task;
	_Atomic int done;
} cr_t;

static int cr_fn(xr_task_t *t)
{
	cr_t *c = t->user;

	atomic_store_explicit(&c->done, 1, memory_order_release);
	return XR_TASK_DONE;
}

static void cr_entry(xr_task_t *t)
{
	cr_t *c = t->user;

	atomic_store_explicit(&c->done, 1, memory_order_release);
}

static int run_create(int l0_fibre, uint64_t n, size_t stack)
{
	cr_t *arr = calloc((size_t)n, sizeof(*arr));
	xr_worker_t *w = xr_worker_create(-1);
	uint64_t t0, t1;

	if (arr == NULL || w == NULL)
	{
		XR_LOGE("create setup failed");
		return 1;
	}
	t0 = xr_tsc();
	for (uint64_t i = 0; i < n; i++)
	{
		if (l0_fibre != 0)
		{
			if (xr_task_init_fibre(&arr[i].task, cr_entry, stack) !=
			    0)
			{
				XR_LOGE("fibre init failed at %" PRIu64, i);
				return 1;
			}
			arr[i].task.user = &arr[i];
		}
		else
		{
			xr_task_init(&arr[i].task, cr_fn, &arr[i]);
		}
		xr_task_spawn(w, &arr[i].task);
	}
	for (uint64_t i = 0; i < n; i++)
	{
		while (atomic_load_explicit(&arr[i].done,
					    memory_order_acquire) == 0)
		{
			xr_cpu_relax();
		}
	}
	/* fibre:entry 返回后由 worker destroy ctx;done 只覆盖 entry 返回前 */
	{
		xr_worker_stats_t st;

		do
		{
			xr_worker_stats(w, &st);
		} while (st.runs < n);
	}
	usleep(1000); /* 让最后一个 ctx destroy 完成 */
	t1 = xr_tsc();

	printf("bench_l0 create: l0=%s n=%" PRIu64 " stack=%zu  %.1f ns/task\n",
	       l0_fibre != 0 ? "fibre" : "stackless", n, stack,
	       (double)xr_tsc_to_ns(t1 - t0) / (double)n);
	print_rss(l0_fibre != 0 ? "fibre" : "stackless");

	xr_worker_destroy(w);
	free(arr);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = "calib";
	int l0_fibre = 0;
	uint64_t ops = 1000000;
	size_t stack = 0;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
		{
			mode = argv[++i];
		}
		else if (strcmp(argv[i], "--l0") == 0 && i + 1 < argc)
		{
			l0_fibre = strcmp(argv[++i], "fibre") == 0;
		}
		else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)
		{
			ops = strtoull(argv[++i], NULL, 0);
		}
		else if (strcmp(argv[i], "--stack") == 0 && i + 1 < argc)
		{
			stack = (size_t)strtoull(argv[++i], NULL, 0);
		}
	}

	xr_time_init();
	if (strcmp(mode, "calib") == 0)
	{
		return run_calib(ops);
	}
	if (strcmp(mode, "create") == 0)
	{
		return run_create(l0_fibre, ops, stack);
	}
	XR_LOGE("unknown mode: %s", mode);
	return 2;
}
