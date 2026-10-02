# xruntime 03 — S4–S6 实施交接(S1–S6 已完成,历史留档)

日期:2026-10-02 · 状态:交接说明 · 上游:`docs/01-plan.md`(计划/判据/台账,
S1–S3 已完成)。**开工前先读 01-plan §0–§3 与本文件 §1/§2。**

## 0. 交接基线

- 仓库:`~/workspace/xruntime`(git repo,独立于 PEL;PEL 在
  `~/workspace/raw-spofer-pel`,其 LSP 报错与本仓无关,勿混改)。
- 分支:`main` = `8c7b734`(S3),`origin/main` = `4401c8b`(S2),**领先 1**;
  `origin` = `git@github.com:qihaoxi/xruntime.git`。未获用户指示不要 push。
- 提交身份(仓库既有作者):`git -c user.name=qihaoxi -c user.email=qihao.xi@foxmail.com commit`。
- 消息风格:`feature: S<N> <中文摘要>`,正文列改动+数字+判定。
- 语言/依赖:C11,`-Wall -Wextra -Werror`,无第三方(libuv/uring 库都不用),
  纯 Linux 原语(epoll/eventfd/futex;io_uring 只用裸 syscall 且可选)。
- 构建/测试/基准(日志自动落盘):
  - `scripts/build.sh [debug|release|asan|tsan|ubsan]`(`XR_CC=clang` 并存多编译器);
  - `scripts/run-tests.sh <profile>`(ctest:smoke/parker/worker);
  - `scripts/run-bench.sh <profile> <bench> [args]`(日志 `bench-logs/`,构建测试 `test-logs/`);
  - `scripts/env-check.sh`(跑基准前必跑:核数/governor/遗留进程/绑核)。
- 门禁(2026-10-02 用户指示分级):默认 `debug` 构建 + 相关 ctest;asan/tsan/
  ubsan 按需;五面全量(gcc/clang Debug、asan、tsan、ubsan 各 3/3)改为按需/
  里程碑,不再每变体强制。
- 本机环境:16 核;governor=**powersave**(无 root 未切,见 §6.4);
  release 下 tsc_hz≈3.79GHz、now_ns=19ns/op、tsc_read=11ns/op(S1 bench_env)。

## 1. 代码结构与关键契约(改动前必读)

| 文件 | 角色 |
|---|---|
| `include/xr/xr_parker.h` / `src/xr_parker.c` | 三态 IDLE/PARKED/NOTIFIED + 单发布者 `pub` 自旋锁;park→CONSUMED/SUSPENDED;unpark→STORED/DELIVER/MERGED |
| `include/xr/xr_task.h` | stackless 续体;返回码 `DONE=0/PARKED=1/RUN_AGAIN=2`;弱句柄 `xr_waker_t{worker,id}` |
| `include/xr/xr_worker.h` / `src/xr_worker.c` | worker 线程+epoll(eventfd)+registry(1024 桶)+ready 队列;V0/V1/V2 flags |
| `tests/test_worker.c` | 跨线程 1 万往返 + 同线程 10 万 ping-pong,flags 0/1/2/3 全覆盖 |
| `bench/bench_roundtrip.c` | B1:`--same-thread`、`--flags`;段 producer_side/consumer_wake/rtt |
| `bench/bench_fanin.c` | B2:`--producers/--wait/--flags`;M→1,统计 eventfd/unpark 与返回码分布 |
| `scripts/*.sh` | build/run-tests/run-bench/env-check |
| `docs/01-plan.md` | 唯一计划/判据/台账(假设 H1–H6、变体 V0–V5、B1–B3) |

关键契约(违反会丢唤醒/竞争,tsan 未必抓到):

1. **parker**:`deliver` 在 unpark 调用线程、持 `pub` 时执行;task 不可迁移
   (owner 固定)。安全红线:通知在 park 判定窗口到达不能丢——改动 `xr_parker`
   必须跑 test_parker 的跨线程 1000 并发用例。
2. **worker 循环顺序**(`src/xr_worker.c:worker_main`):
   pop→run;空则查 stop;**先 `sleeping=1`(release)再复核队列**;仍空才
   `epoll_wait`;返回后 `sleeping=0`→read eventfd→`wake_pending=0`→回到 drain。
   这个"先置 sleeping 再复核"是 V2 门控不丢唤醒的关键,勿简化。
3. **wake_pending**:libuv async-pending 式合并;只在 read eventfd 之后清零,
   且在下一轮 drain 之前;否则丢唤醒。
4. **flags**(`xr_worker_set_flags`,必须在 `xr_task_spawn` 前调用):
   `1=DIRECT`(同线程跳过 transport,TLS 识别)、`2=GATE`(未 sleeping 不写
   eventfd)。deliver 内以 relaxed 读一次。
5. **task 唯一入队不变式**:parker 状态机保证同一 task 在任一时刻至多一个
   ready 条目(DELIVER/STORED 要求 state≠NOTIFIED)→ V3 环形队列可依赖此
   性质,但新增路径必须复核。
6. **测量口径**:B1 cross=外部线程串行一次一 op;same-thread=同 worker 双
   task 互踢(共享计数到 n 结束,**不要用两 task hops 求和当停止条件**——
   S3 曾因此误判,见 §6.1)。B2 wait=请求-响应口径(复刻 PEL D5);unpaced=
   合并上限形态。

## 2. 已沉淀基线数字(S1–S3,release,3 轮 median-p50)

- B1 cross-thread(flags=0,ops=20 万,wkcpu=1/prodcpu=2):
  producer_side **1.29µs**、consumer_wake **1.78µs**、RTT **3.08µs**
  (avg 3.3µs;p99 波动 4–12µs;neg_delta 50–100,已钳 0);wake_writes≈20 万。
- B1 same-thread(flags=0):producer_side 30–50ns、consumer_wake 10–20ns、
  RTT **50–70ns**;**wake_writes=2/40 万 unpark**(pending 合并已消 transport)。
- B2 wait(flags=0):M=1 **0.29 Mops/s,eventfd/op 0.987–0.991**;
  M=4 1.07–1.09 Mops/s、0.248–0.249;M=8 1.98–2.04 Mops/s、**0.1244–0.1245**。
- B2 unpaced(flags=0):M=1 90–105 Mops/s、eventfd/op 0.002;M=4 15.7–16.6、
  0.014–0.016;M=8 11.7–11.8 Mops/s、0.019。
- V1/V2 A/B 结论:S3 全在噪声内 → **V1/V2 不作默认、不迁移 PEL**;H1/H4
  证伪(同线程 transport 已被 pending 消掉;跨线程瓶颈是 cache-line 传输+
  epoll 唤醒,不是 eventfd 写/门控)。同线程 60ns vs 跨线程 3µs(50×)。
- PEL 对照(doc151 §4.3,勿当目标):eventfd/req http/echo c=1 2.00/1.00、
  c=16 0.38/0.13、c=256 0.025/0.008;PEL echo 往返 ~11.5µs=4 对 park/wake。

## 3. S4:V3 lock-free MPSC / V4 waker lifetime(H2/H3/H5)

> 状态(2026-10-03):V4a ✅ 未超噪声(不作默认);V3 ✅ 同线程 hop
> RTT p50 50→20ns(-45%),跨线程/B2 噪声内;H2/H3 证伪。GATE×MPSC 丢唤醒
> 已修为"MPSC|GATE 时 GATE 自动失效"(constraints §4.2)。结果见 01-plan
> S4 验证;下一步 S5。

### 3.1 目标
同线程 hop 60ns 与跨线程 producer_side 1.29µs 里,mutex+registry 是固定税;
砍掉/替换它们,看能否超噪声。**建议顺序:V4a → V3;V4b 仅在生命期出问题时做。**

### 3.2 V4a 直接 waker(先做)
- flag:`XR_WORKER_WAKER_DIRECT = 1u << 3`。
- `xr_task_spawn` 中按 flags 选择 deliver 上下文:
  - 默认(现状):`xr_parker_init(&t->parker, task_deliver, &t->waker)`;
  - V4a:`xr_parker_init(&t->parker, task_deliver_direct, t)`(ctx=task 指针),
    且跳过 `reg_insert_locked`(registry 不再被查)。
- 新函数 `task_deliver_direct(xr_parker_t *p, uint64_t payload, void *ctx)`:
  `xr_task_t *t=ctx; xr_worker_t *w=t->owner;` push ready(仍走 V0 mutex 队列,
  单变量)→ 沿用现有 DIRECT/GATE/worker_wake 逻辑。
- **生命期不变式**:直接指针要求 task 对象活过所有 in-flight unpark。沙盒里
  task 为栈对象、producer 线程 join 后才 `xr_worker_destroy`,安全;文档里
  必须写明该前置条件。
- 预期:同线程 hop、跨线程 producer_side 至少有一处 ≥5%。判定见 §3.5。

### 3.3 V4b 引用计数 waker(可选)
仅在需要形式化生命期时做:claim 成功时 `refs.fetch_add`,deliver 结束
`fetch_sub`,task 摘除前自旋等 0。沙盒静态 task 用不上,默认不做,做了要留档
说明"为真实运行时预研"。

### 3.4 V3 Vyukov MPSC ready 队列
- flag:`XR_WORKER_MPSC = 1u << 2`;新文件 `include/xr/xr_mpsc.h`+`src/xr_mpsc.c`。
- 有界环形 4096(2 的幂),Vyukov:
  ```
  slot { _Atomic uint64_t seq; xr_task_t *t; }
  push: pos=tail.fetch_add(1); s=&slots[pos&mask];
        bounded spin 等 s->seq==pos;超限→溢出链(worker lock 下的链表);
        s->t=task; s->seq.store(pos+1, release);
  pop:  pos=head.load; s=&slots[pos&mask];
        if (s->seq.load(acquire)!=pos+1) return NULL;
        task=s->t; s->seq.store(pos+capacity, release); head.store(pos+1);
  ```
- `worker_pop`:先 MPSC,再溢出链;`RUN_AGAIN` 重排与 deliver 的 push 同路径。
- 依赖 §1.5 的"task 至多一个条目"不变式;新增测试:`K` 个 task 高频
  unpark/drain 压力(可复用 test_worker ping-pong + B2 unpaced)。
- 验收:B2 unpaced M=8(基线 11.7–11.8 Mops/s)与同线程 hop;必要时
  `perf record -e cycles` 看锁等待。

### 3.5 S4 执行流程与判定(每个变体)
1. 实现(默认 flag=0 行为不变)→ 2. 扩 test_worker 覆盖该 flag →
3. debug 构建 + 相关测试绿(并发改动按需加 tsan;五面全量按里程碑,见 §0)
→ 4. 同窗 A/B:flags=0 与变体各 3 轮 median,同机同 session,
   B1 same/cross + B2 wait/unpaced 至少各一组 → 5. 超噪声(≥5% 或延迟分布
   明确前移)才保留默认;否则保留 flag 但标"未超噪声",台账留档。
每步更新 `docs/01-plan.md` 台账 S4 与 README 状态表。

## 4. S5:V5 transport 备选(H6)

> 状态(2026-10-03):V5 futex ✅。B1 cross RTT 3196→390ns(-88%)、B2 wait
> 1.99→4.62 Mops/s(+132%)、unpaced -11%(延迟换吞吐);H6 证实(transport
> 是跨线程每对 ~3µs 主项)。单字协议/契约见 01-plan S5 验证与
> constraints §4.2;下一步 S6。

- flag:`XR_WORKER_FUTEX = 1u << 5`(futex 模式替代 epoll 主循环;本沙盒暂无
  真实 IO,事件面只有唤醒)。
- futex 协议(与 sleeping/pending 协议配套,勿另起炉灶):
  ```
  worker: store(fut_word,0,release); if stop break;
          futex(&fut_word, FUTEX_WAIT_PRIVATE, 0, &ts{100ms});  // 超时只为看 stop
  deliver(仅 sleeping 时): store(fut_word,1,release);
          futex(&fut_word, FUTEX_WAKE_PRIVATE, 1, NULL);
  ```
  保留 `sleeping` 先置后复核顺序(§1.2);futex 模式下 pending 合并可省,
  但先原样保留以便对照。
- 可选(记录理由,不默认做):
  - V5b io_uring `IORING_OP_MSG_RING`:裸 `io_uring_setup/enter`(不用
    liburing),内核 ≥5.18;没有第二个 ring 时意义有限,除非 futex/eventfd
    对照显示内核路径是主项;
  - V5c 批量 wake:pending 合并已把 wait M=8 压到 0.12,预期无收益;
    仅在 S4 显示 transport 占比回升时再试。
- 验收:B1 cross consumer_wake/RTT(基线 1.78µs/3.08µs)+
  `strace -c -f -e trace=futex,write,epoll_wait` 计数;≥5% 才保留。

## 5. S6:B3 高并发合成 echo + 总报告

> 状态(2026-10-03):S6 ✅。`bench/bench_echo.c`(spin/park 双口径)落地;
> B3 结果与三问决策见 `docs/04-final-report.md` 与 01-plan S6 验证。
> S1–S6 全部完成;后续候选:transport 回灌 PEL 的设计评审、L0 栈切换
> 对照轮(01 §0.5)——**L0 轮已于 S7 完成**(01 S7 / 02 §0.5.5 / 04 §3.6)。

### 5.1 B3 设计
- `bench/bench_echo.c`:K 个 conn task(单 worker;K=1/16/256/4096),M 个
  driver 线程(M=1/4/16);driver 选 conn=(tid+seq)%K,投 req(每 conn 一个
  SPSC 队列或 mutex 队列),unpark conn,等 ack。
- ack 两种口径:`--ack=spin`(driver 自旋等 processed 计数,上限估计)与
  `--ack=park`(driver 自带 parker,conn 处理完 unpark driver——忠实 PEL
  阻塞 echo 的 2 对 park/wake)。
- conn step:唤醒后 drain 本 conn 请求(可批量),处理计数,ack driver。
- 指标:rps、req→ack p50/p99、eventfd(或 futex)/req、parks/req、runs/req。
- 形态对照(非严格跨项目对比):PEL doc151 §4.3 与 rust 公开数;重点看
  随 K/M 的拐点与合并率趋势。

### 5.2 总报告(建议 `docs/04-final-report.md`)
必须回答:
1. PEL 残差是否在 wake 机制?(S3 证据:transport 已被合并,跨线程每对 ~3µs
   是传输+唤醒,同线程仅 60ns——直投类优化只在同线程有空间)
2. 哪个变体在何规模下超 V0 ≥5%?给出 B1/B2/B3 原始日志位置。
3. 迁移建议:增量收敛点回灌 PEL vs 另立运行时。红线:按 AGENTS.md,
   PEL 核心目录(含 scheduler/parker/channel)任何改动需书面设计+asan/
   ubsan-extra/tsan 三面;doc155 §7.7 反绕过纪律同样适用;沙盒结论不构成
   改 PEL 核心目录的依据。

## 6. 纪律与坑(S1–S3 教训,勿重蹈)

1. **测试停止条件**:同线程 ping-pong 不能用"两 task hops 之和 ≥n"退出
   (链会跑过 n,sum 可达 n+1);用共享 total 计数器,到 n 的那棒 DONE。
2. **V2 门控丢唤醒窗口**:必须 `sleeping=1` 之后再做最后一次队列复核;
   生产者顺序是"入队(unlock)→ 读 sleeping";两者缺一必丢唤醒(tsan 不报)。
3. **同线程 V0 wake_writes≈2 不是 V1 的功劳**:队列不空时 worker 不 poll,
   pending 永不清零;别把该数当变体收益。
4. **governor=powersave**:只信 ≥5% 或分布明确前移;A/B 的 flags=0 必须与
   变体在同一 binary/同 session 重测(跨 session 的 S2 基线只作参考)。
5. **跨线程 TSC 负差**:t2<t1 属正常,聚合时钳 0 并记 neg_delta;比较看
   p50/avg,别被 max 带偏。
6. **flags 设置时机**:`xr_worker_set_flags` 必须在 spawn 前;V4a 起 flags
   还会决定 deliver 函数/registry 注册,改动时保持"默认 0=纯 V0"。
7. 日志/中间产物落 `test-logs/`、`bench-logs/`(gitignore);构建目录
   `build-<profile>[-clang]`;失败先取 `-latest.log`,不要为拿日志重跑。
8. 提交规范:类型前缀+中文正文;只提交变体代码/测试/台账/README;不 push
   除非用户明说。

## 7. 下个会话第一步(检查表)

1. `cd ~/workspace/xruntime && git log --oneline -4` → 期望 `1677514`(交接)
   或其后 S4 提交。
2. 读 `docs/01-plan.md` §0–§3 + 本文件 §1–§2。
3. `./scripts/env-check.sh`;跑 debug 基线确认绿:
   `./scripts/build.sh debug && ./scripts/run-tests.sh debug`
   (sanitizer 按需;五面全量按里程碑,见 §0)。
4. 同 session 重测 flags=0 对照:B1 same-thread、B1 cross-thread、
   B2 wait/unpaced M=1/4/8(命令见 README「构建与基准」)。
5. S1–S6 全部完成(S3–S6 见 §3/§4/§5 状态,总报告
   `docs/04-final-report.md`);后续候选:transport 回灌 PEL 设计评审、
   L0 栈切换对照轮(01 §0.5)——L0 轮已于 S7 完成(01 S7 / 02 §0.5.5 /
   04 §3.6);**S8 修正了 §5 的 B3 park 口径**(bench_echo `drv_step`
   通知双消费 bug,S6 park 数字已按修正值改写,见 01 S6/S8)。

并行事项(不属本仓):PEL 侧 macOS 构建修复已推两个 commit
(`2ce8a740` vmem MADV_NOHUGEPAGE 守卫、`3ae00936` C11 标签后声明),等用户
macOS 重编反馈,若有新平台报错继续逐个修。
