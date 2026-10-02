# xruntime

**parker wakeup 机制沙盒**——把 park→unpark→resume 全链拆到段级成本,逐变体
A/B,为"是否值得另立高上限运行时 / 能否回灌 PEL"提供同窗测量证据。

- 定位:机制实验仓,**非产品运行时**,不引入 libuv/第三方,只用 Linux 原语
  (epoll/eventfd/futex,io_uring 可选);
- 背景:PEL 性能线收口后,残差被钉为 **stackful 阻塞模型税**与 **事件面/wakeup
  形态差**(echo c≥16 0.37–0.69×rust;UDP 往返 ~11.5µs=4 对 park/wake);
- 详细计划、假设 H1–H6、变体矩阵 V0–V5、验收判据见
  [`docs/00-plan.md`](docs/00-plan.md)(唯一正文,随 sub-task 更新台账)。

## 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| S1 | 骨架 + parker 三态最小实现 | ✅ 2026-10-02 |
| S2 | V0 基线(worker/registry/mutex 队列)+ B1/B2 | ✅ 2026-10-02 |
| S3 | V1 同线程直投 / V2 门控 wake | ✅ 2026-10-02(未超噪声,默认关) |
| S4 | V3 lock-free 队列 / V4 waker lifetime | ✅ 2026-10-03(V4a 未超噪声;V3 同线程 hop -45%,跨线程/B2 噪声内) |
| S5 | V5 transport 备选(futex/io_uring/批量) | ✅ 2026-10-03(futex:RTT -88%、wait +132%;unpaced -11%) |
| S6 | 高并发合成 echo + 总报告/决策建议 | ✅ 2026-10-03(见 `docs/02-final-report.md`) |

## parker 模型

```
state: IDLE --park CAS--> PARKED --unpark CAS--> NOTIFIED --park CAS--> IDLE
             \--unpark CAS--> NOTIFIED(提前通知,无 waiter)
```

- `xr_parker_park()` → `CONSUMED`(已有通知,不挂起)| `SUSPENDED`(已置
  PARKED,调用方负责挂起);
- `xr_parker_unpark()` → `STORED`(IDLE→NOTIFIED)| `DELIVER`(PARKED→
  NOTIFIED,回调 deliver)| `MERGED`(已有未消费通知,不重复唤醒);
- 单发布者(pub CAS 自旋锁)+ payload 先写后迁状态;同一 parker 单等待者、
  unpark 多生产者并发安全;deliver 在 unpark 线程持 pub 执行、不得重入。
- 形态对齐 PEL `doc00 §5.2 / doc97`,刻意去掉 fibrage/registry 耦合,便于
  单变量替换唤醒链各段(registry、队列、transport)。

## 目录

```
include/xr/   xr_parker.h(三态协议) · xr_task.h(stackless 续体/弱句柄) ·
              xr_worker.h(V0 worker) · xr_time.h · xr_log.h · xr_env.h(绑核)
src/          上述实现;xr_parker.c=机制核心,xr_worker.c=V0 唤醒链
tests/        test_parker · test_worker(V0 万次往返) · test_smoke
bench/        bench_env(环境体检) · bench_roundtrip(B1 延迟分解) ·
              bench_fanin(B2 唤醒税,--wait=请求-响应口径)
scripts/      build.sh · run-tests.sh · run-bench.sh · env-check.sh
docs/         00-plan.md(计划/台账/判据) · 01-handoff-s4-s6.md(S4–S6 交接)
test-logs/    构建与测试日志(gitignore)  bench-logs/  基准日志(gitignore)
```

## 构建与测试

```bash
scripts/build.sh debug            # gcc Debug → build-debug/
XR_CC=clang scripts/build.sh debug   # clang Debug → build-debug-clang/
scripts/build.sh asan             # 另支持 release/asan/tsan/ubsan
scripts/run-tests.sh debug        # ctest,失败样例见日志
```

- 构建/测试日志自动落盘 `test-logs/build-<profile>-<ts>.log` 与
  `test-logs/test-<profile>-<ts>.log`(附 `-latest.log` 软链);
- 全目标 `-Wall -Wextra -Werror`(gcc/clang 双编译器纪律);
- sanitizer 面:`asan` / `tsan` / `ubsan`,与 debug 共用 `scripts/*.sh`。

## 基准

```bash
scripts/build.sh release
scripts/run-bench.sh release bench_env --iters 200000 [--cpu N]

# B1 唤醒延迟分解(跨线程,串行;--same-thread=同 worker 双 task 互踢)
scripts/run-bench.sh release bench_roundtrip --ops 200000 --wkcpu 1 --prodcpu 2
scripts/run-bench.sh release bench_roundtrip --ops 200000 --same-thread

# B2 每请求唤醒税(fan-in;--wait=请求-响应口径,复刻 PEL D5)
scripts/run-bench.sh release bench_fanin --producers 8 --ops 200000 --wkcpu 1 --prodcpu 2 --wait

# 变体开关(A/B):--flags 0=V0 · 1=V1 同线程直投 · 2=V2 parked 门控 · 3=both
```

测量纪律(沿用 PEL):先跑 `scripts/env-check.sh`(governor=performance、无遗留
进程、负载);生产者/消费者分核绑 CPU(`--cpu`);warmup + ≥3s 测量、3 轮
median;变体间**同窗单变量 A/B**;结论须超轮间噪声(≥5% 或延迟分布明确前移)
才保留,否则回退留档。

## 边界

- stackless 续体先行的结论不直接等价 PEL stackful fibre(补栈切换对照轮);
- 沙盒结论迁回 PEL 核心目录须重新评审(PEL doc155 §7.7 反绕过纪律):
  只认同窗数字 + perf/strace 证据,纯理论结论不立项。
