#ifndef XR_ENV_H
#define XR_ENV_H

/* 运行环境/绑核工具(测量纪律:生产者与消费者分核)。 */

int xr_cpu_count(void);

/* 0=成功,负=errno(sched_setaffinity)。 */
int xr_pin_to_cpu(int cpu);

/* 当前线程所在 CPU,失败 -1。 */
int xr_current_cpu(void);

/* 绑核失败直接退出(基准用:测量环境不满足即 fail-fast)。 */
void xr_pin_or_die(int cpu);

#endif
