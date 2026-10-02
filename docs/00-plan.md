# xruntime 00 — parker wakeup 机制沙盒:计划与判据

日期:2026-10-02 · 状态:计划(待评审) · 定位:机制实验仓,非产品运行时

## 0. 背景与定位

来源:PEL 性能线收口后,残差被钉为两点——**stackful 阻塞模型税**与
**事件面/wakeup 形态差**(doc148 ledger:echo c≥16 0.37–0.69×rust;doc156
§5:UDP 往返 ~11.5µs=4 对 park/wake;doc50/51/25 已否决/挂起 work stealing
与非对称运行时)。本仓做**机制沙盒**:先把 wake→resume 全链拆到段级成本,
再逐变体 A/B,产出可回灌 PEL 或支撑"另立运行时"决策的证据。

非目标(第一阶段):
- 不做完整运行时、不引入 fibre/vstack/IO 协议、不碰 PEL 仓;
- 不引入 libuv 与第三方(纯 Linux 原语:epoll/eventfd/futex,io_uring 可选);
- 不承接 PEL 的 owner-guard/registry 契约——正是要对比替代方案。

PEL 事实基线(对照,勿当目标数字):
- park 快路径:1 个 SEQ_CST CAS 无挂起;慢路径用户态栈切换(无 syscall)。
- unpark:claim 选举 CAS + payload + open-door CAS → registry 弱句柄解析
  (owner sched `uv_mutex` + 1024 桶 hash)→ SUSPENDED→READY + ready 队列
  (mutex 侵入链表)→ `uv_async_send` **恒发**(doc151 §4.3;去掉跳过是因
  uv_write 回调时序,`pel_scheduler.c:679-696`)。
- eventfd/req:kprobe 实测 http c=1 2.00、echo c=1 1.00;c=16 0.38/0.13;
  c=256 0.025/0.008(doc151 §4.3)——高并发已被 libuv 合并,**瓶颈不在 syscall**。
- 残差归因:每请求 2 对阻塞 park/wake 的模型税(D5 定向投递 ❌ 无靶、
  D8 同线程内联 drain 实现后回退、D9 fire-and-forget 否决)。

## 1. 待验证假设(H1–H6)

| # | 假设 | 对应变体 |
|---|---|---|
| H1 | 同线程 unpark 仍付 transport+registry+mutex,安全点直投可消除 | V1 |
| H2 | ready 队列 mutex 是多生产者跨线程争用热点 | V3 |
| H3 | registry 弱句柄 hash+锁是每唤醒固定成本,可被引用计数 waker/单槽直投替代 | V4 |
| H4 | target 未 park 时发 wake 是纯浪费,parked-flag 门控可省 | V2 |
| H5 | 跨线程 cache-line bounce(状态字/队列头/registry)决定扩展性 | V3/V4 |
| H6 | 高并发吞吐瓶颈可能转移到 ready drain 粒度/loop tick 批次与公平性 | V3/B3 |

## 2. 沙盒设计

组件(最小集):
- `xr_worker`:线程 + epoll + eventfd + ready 队列;绑核;`run_ready()` 在
  poll 前后各 drain 一次(定义安全点)。
- `xr_task`:stackless 续体(函数指针+状态),先不引入栈切换——resume 成本
  预期 ~百 ns 级,不改变排序结论;若差异进噪声再补栈切换变体。
- `xr_parker`:三态 IDLE/PARKED/NOTIFIED + claim/publish/open-door 协议,
  语义对齐 PEL doc00 §5.2(便于对照,不照搬生命周期)。
- waker 句柄/lifetime:基线=per-worker registry + 弱句柄;变体=引用计数。

变体矩阵(单变量串行,一次只动一处):
- **V0 基线**:mutex 侵入 ready 队列 + registry hash + 无条件 eventfd(复刻 PEL 形态)。
- **V1 同线程安全点直投**:owner==current 且安全点时只入队不 transport。
- **V2 parked-flag 门控 wake**:未挂起仅入队;挂起才 transport。
- **V3 lock-free MPSC ready 队列**:Vyukov 有界环形 + 溢出链;生产者无锁。
- **V4 waker lifetime**:引用计数 waker(无 registry 查找)或 per-parker 单槽 fast path。
- **V5 transport 备选**:futex / eventfd-semaphore / io_uring MSG_RING / 批量 wake(内核支持单独记录)。

安全红线:每变体过 asan/tsan;丢唤醒(通知在 park 判定窗口内到达)需专项
stress;句柄失效窗口按"miss=drop 可解释"设计。

## 3. 基准与判据

方法学(沿用 PEL 纪律):绑核(`taskset`,生产者/消费者分核)、
governor=performance、warmup 1s、measure ≥3s、3 轮 median、单变量同窗 A/B、
逐轮日志落盘 `bench-logs/`;每轮跑前查环境(无遗留进程/端口/抢核)。

- **B1 wake→resume 延迟分解**(ns/op,p50/p99):同线程、跨线程各一组;
  分段计时:claim / publish / resolve / enqueue / transport / tick / resume。
- **B2 每请求唤醒税**:fan-in M→1、fan-out 1→M;`strace -c`/perf 计
  eventfd 写、futex 调用、锁等待;目标是先把 V0 复刻到 PEL D5 形态
  (c≥16 ×0.13/0.008 量级),再逐变体下压。
- **B3 高并发合成 echo**:K 个 task(recv park→投递→处理→send park→投递),
  K=1/16/256/4K;产出 rps/p50/p99/每 req 唤醒税;与 PEL/rust 公开数字作
  **形态参照**(非严格跨项目对比)。

判定:变体须超出轮间噪声(同窗 ≥5% 或延迟分布明确前移)才保留,否则回退
并留档(规则数净减/持平原则)。

## 4. 步骤台账

> S4–S6 的逐变体接口/实现步骤/验收/坑,见 `docs/01-handoff-s4-s6.md`(交接文档)。

| S | 内容 | 判定物 | 状态 |
|---|---|---|---|
| S1 | 骨架:CMake/目录/日志规范/绑核与 sanitizer 脚本(+ parker 三态最小实现前移) | 构建可跑 | ✅ 2026-10-02 |
| S2 | V0 基线 + B1/B2,复刻 PEL 唤醒形态 | 基线数字 | ✅ 2026-10-02 |
| S3 | V1/V2(H1/H4),目标:同线程链零 transport | A/B 报告 | ✅ 2026-10-02 |
| S4 | V3/V4(H2/H3/H5),队列与 lifetime | A/B 报告 | ⬜ |
| S5 | V5 transport 备选(H6) | A/B 报告 | ⬜ |
| S6 | B3 高并发合成 + 总报告:回灌 PEL / 另立运行时 判据 | 决策建议 | ⬜ |

S1 验证(2026-10-02,本机 16 核):
- 构建/测试五面全绿:gcc Debug、clang Debug、asan、tsan、ubsan(各 2/2);
- `xr_parker` 单测覆盖 CONSUMED/SUSPENDED、STORED/MERGED/DELIVER,跨线程
  1000 并发 unpark 仅 1 次 DELIVER;
- `bench_env`(release):tsc_hz≈3.79GHz,now_ns=19ns/op,tsc_read=11ns/op;
- 脚本:`build.sh [debug|release|asan|tsan|ubsan]`(`XR_CC=clang` 多编译器并存,
  `run-tests.sh`/`run-bench.sh`/`env-check.sh`),日志落 `test-logs/`、`bench-logs/`。

S2 验证(2026-10-02,本机 16 核,release;注意:governor=powersave 未切换,
但 3 轮 p50 逐 ns 一致,量级结论可用):
- V0 链路落地:xr_worker(epoll+eventfd+mutex registry/ready 队列)+
  xr_task(stackless 续体)+ 弱句柄(id)deliver;test_worker 1 万往返五面绿。
- B1 跨线程(ops=20 万,wkcpu=1/prodcpu=2,3 轮 median-p50):
  producer_side(claim/publish/deliver+registry+队列+eventfd)=1.24µs;
  consumer_wake(传输+tick+恢复)=1.75µs;RTT=3.0µs(p99 4.7µs,avg 3.2µs)。
  单对 park/wake≈3µs,与 PEL ~11.5µs/往返=4 对(≈2.9µs/对)同量级。
- B2 wait 模式(请求-响应口径,复刻 D5):eventfd/unpark M=1 **0.995**
  (PEL D5 echo c=1=1.00);M=4 0.250;M=8 **0.118~0.125**(PEL c=16 echo=0.13)。
  无节流 unpaced:M=1 即 99.7% merged(eventfd/op 0.002)=latch 合并上限形态。
- 结论:V1/V2 的对照基线就位;"链上单对"成本已与 PEL 每对接近,后续变体
  净收益可直接在同一 B1/B2 口径上判定。

S3 验证(2026-10-02,本机 16 核,release;governor=powersave):
- 变体接入:`XR_WORKER_DIRECT`(V1)/`XR_WORKER_GATE`(V2)运行期 flags
  (默认 0=V0);test_worker 覆盖 flags 0/1/2/3 ×(跨线程 1 万 + 同线程
  10 万 ping-pong),五面全绿。
- B1 same-thread(20 万 hop,3 轮 median-p50):RTT 50~70ns;flags 0/1/2/3
  全在噪声内;V0 wake_writes=2/40 万 unpark(pending 合并已把 eventfd 压到 0)。
- B1 cross-thread:flags 0/2/3 均为 producer_side 1.29µs + consumer_wake
  1.78µs = RTT 3.08µs;gate 仅省 20~30/20 万次写,无延迟差。
- B2 wait(D5 口径)flags0 vs 2:eventfd/unpark M=1 0.987~0.991 vs
  0.968~0.983;M=4 0.249 vs 0.249;M=8 0.1245 vs 0.1245;吞吐同噪声。
- B2 unpaced:M=8 0.0191 vs 0.018~0.019,吞吐 11.7~12.2 Mops/s 同噪声。
- 判定:V1/V2 均未超噪声,**不保留为默认、不迁移 PEL**(flags 默认 0,
  仅留作 S4 对照开关);H1/H4 在本模型下证伪——同线程 transport 已被
  pending 合并消掉(40 万 op 仅 2 写),跨线程瓶颈是 cache-line 传输 +
  epoll 唤醒(~3µs/对),不是 eventfd 写/门控能省的。同线程 60ns vs
  跨线程 3µs 的 50× 差说明直投收益只在"把工作搬回同线程"时才存在
  (呼应 PEL D8 同线程内联 drain 回退)。

## 5. 风险与边界

- stackless 续体 ≠ PEL stackful fibre:resume 成本差 ~百 ns,预期不改排序;
  若结论吃紧,补栈切换(自研或 ucontext)对照轮。
- 无 libuv 的 loop 与 uv 合并/回调时序不同:V1 同线程直投在 PEL 曾被 D8
  否决(帧寿命契约),**结论迁移回 PEL 必须重新评审**(doc155 §7.7 反绕过
  纪律同样适用);沙盒结果不直接构成对 PEL 核心目录的修改依据。
- 只认同窗对照数字 + perf/strace 证据;纯理论结论不立项。

## 6. 对照资料

- PEL:`docs/148` §0.1–0.3/§3.1/ledger · `docs/151` §4.3 · `docs/152` §7 ·
  `docs/156` §4–5 · `docs/160/161` · `docs/00-unified-lifecycle-design.md`
  §0/§5.2 · `src/scheduler/pel_parker.h` · `src/eventloop/uv_channel.c`。
- 外部:Tokio Parker/scheduler 设计、Vyukov MPSC、futex(2)、io_uring
  `IORING_OP_MSG_RING`。
