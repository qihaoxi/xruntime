#include "xr/xr_mpsc.h"

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

int main(void)
{
	xr_mpsc_t *m;
	xr_task_t tasks[8] = { 0 };

	CHECK(xr_mpsc_create(0) == NULL);
	CHECK(xr_mpsc_create(3) == NULL);

	m = xr_mpsc_create(4);
	CHECK(m != NULL);
	if (m == NULL)
	{
		return 1;
	}

	/* 空队列 */
	CHECK(xr_mpsc_pop(m) == NULL);

	/* 填满 4 位,第 5 位预检满 → false(走溢出链) */
	for (int i = 0; i < 4; i++)
	{
		CHECK(xr_mpsc_try_push(m, &tasks[i]));
	}
	CHECK(!xr_mpsc_try_push(m, &tasks[4]));

	/* FIFO 出队 */
	for (int i = 0; i < 4; i++)
	{
		CHECK(xr_mpsc_pop(m) == &tasks[i]);
	}
	CHECK(xr_mpsc_pop(m) == NULL);

	/* 绕圈复用:同槽位 seq 进入下一圈 */
	for (int i = 0; i < 4; i++)
	{
		CHECK(xr_mpsc_try_push(m, &tasks[4 + i]));
	}
	for (int i = 0; i < 4; i++)
	{
		CHECK(xr_mpsc_pop(m) == &tasks[4 + i]);
	}
	CHECK(xr_mpsc_pop(m) == NULL);

	xr_mpsc_destroy(m);

	if (failures != 0)
	{
		fprintf(stderr, "mpsc: %d failure(s)\n", failures);
		return 1;
	}
	printf("mpsc: OK (full/wrap/FIFO)\n");
	return 0;
}
