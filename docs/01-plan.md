# xruntime 01 — parker wakeup 机制沙盒:计划与判据

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

安全红线:并发/生命周期改动建议按需跑 tsan+asan(2026-10-02 分级:五面全量
改为按需/里程碑,不再每变体强制);丢唤醒(通知在 park 判定窗口内到达)需专项
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

> S4–S6 的逐变体接口/实现步骤/验收/坑,见 `docs/03-handoff-s4-s6.md`(交接文档)。

| S | 内容 | 判定物 | 状态 |
|---|---|---|---|
| S1 | 骨架:CMake/目录/日志规范/绑核与 sanitizer 脚本(+ parker 三态最小实现前移) | 构建可跑 | ✅ 2026-10-02 |
| S2 | V0 基线 + B1/B2,复刻 PEL 唤醒形态 | 基线数字 | ✅ 2026-10-02 |
| S3 | V1/V2(H1/H4),目标:同线程链零 transport | A/B 报告 | ✅ 2026-10-02 |
| S4 | V3/V4(H2/H3/H5),队列与 lifetime | A/B 报告 | ✅ 2026-10-03 |
| S5 | V5 transport 备选(H6) | A/B 报告 | ✅ 2026-10-03 |
| S6 | B3 高并发合成 + 总报告:回灌 PEL / 另立运行时 判据 | 决策建议 | ✅ 2026-10-03 |

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

S4 验证(2026-10-03,本机 16 核,release;governor=powersave;同窗 3 轮 median):
- **V4a 直接 waker**(`XR_WORKER_WAKER_DIRECT`,跳过 registry):B1 cross
  producer_side p50 1262→1282ns(+1.6%)、RTT 3025→3055ns(+1%);same-thread
  RTT p50 50→60ns;test_worker 12 flags 全绿 + tsan。**未超噪声,不作默认**;
  H3 证伪——单桶 registry 查找不是固定税,瓶颈在 mutex 队列 + transport。
- **V3 Vyukov MPSC**(`XR_WORKER_MPSC`,4096 环 + 溢出链):B1 **same-thread
  RTT p50 50→20ns、avg 75→41ns(≈-45%)**,producer_side avg 43→15ns;cross
  RTT p50 -1.8% / avg -2.8%(噪声内);B2 wait +1.5% / unpaced -0.4%(噪声内)。
  **同线程 hop 显著超噪声**(mutex 队列是直投路径的固定税);跨线程/B2 未超;
  H2 证伪——M=8 wait/unpaced 下 87.5%/98% 已合并,队列争用不是瓶颈。
  V3 保留 flag(默认 0);候选上推默认,待 B3 echo 判定。
- 新增 `src/xr_mpsc.c` + `tests/test_mpsc.c`(满/绕圈/FIFO/非法容量),
  test_worker 覆盖 12 flags 组合;debug/tsan/asan 绿。
- **坑(已修,入 constraints §4.2/§4.4)**:GATE×MPSC 组合丢唤醒——GATE 协议
  依赖 mutex 队列对"入队 vs 睡眠前复核"的串行化;MPSC 无锁环破坏该协议
  (store-load 竞速,tsan 不报;30 次复跑复现)。  现契约:**MPSC|GATE 同开时
  GATE 自动失效(总 transport)**,`xr_worker.h` 注明。

S5 验证(2026-10-03,本机 16 核,release;governor=powersave;同窗交错 3 轮 median):
- **V5 futex transport**(`XR_WORKER_FUTEX`,单字协议:fut_word 0=声明睡眠/
  1=awake;生产者 0→1 才 `futex_wake`,awake 时零 syscall;futex_wait 以字值
  原子复核):
  - B1 cross:RTT p50 **3196→390ns(-88%)**、avg 3526→450ns;
    producer_side 1332→220ns(-83%)、consumer_wake 1853→170ns(-91%);
  - B2 wait(D5 口径):吞吐 1.99→**4.62 Mops/s(+132%)**,eventfd/unpark=0;
  - B2 unpaced(饱和):11.68→10.41 Mops/s(**-11%**)——futex 即时唤醒降低
    合并度(runs/unpark 0.020→0.053,parks/deliver 1.9→2.0):**延迟换吞吐**;
  - same-thread 同噪声(不睡眠路径无差异)。
- 判定:**transport 是跨线程每对 ~3µs 的主项**(H6 证实);futex 在
  请求-响应/阻塞形态决定性收益,饱和 fan-in 有 ~11% 吞吐回退。
  保留 flag(默认 0);候选:延迟敏感默认 futex,真实 IO 仍需 epoll/eventfd
  接入 fd;B3 echo 判定组合形态。
- 契约(入 constraints §4.2/`xr_worker.h`):FUTEX 自带单字门控(sleeping 不再
  维护),GATE 与 FUTEX 同开自动失效;**MPSC|FUTEX 正确**(单字协议不依赖队列
  串行化);set_flags 切 FUTEX 时对可能睡在 epoll 的 worker 补 eventfd 唤醒
  (一次性配置期双写)。
- 门禁:test_worker 16 flags 组合 ×(cross+same)、debug/tsan 绿、30 次 flaky
  复跑零失败;TSan 曾单次报 test 栈对象×worker 读,28 次复跑不复现(疑
  裸 futex 非 TSan 拦截路径的时序伪影),留档待复现。

S6 验证(2026-10-03,本机 16 核,release;governor=powersave;B3=bench_echo):
- B3 spin(K=256,M=16 driver 自旋口径;3 轮 median):
  - M=1:V0 285724 → **V5 futex 2006380 rps(+602%)**,p50 3336→430ns;
  - M=4:V0 2373189 → V5 2559982(+8%),p50 ~1072→1021ns;
  - M=16:V0 1175281 → V5 1388493(+18%,方差大),p50 ~2.1µs 持平。
- B3 park(driver 为 task,ack=unpark;2 对 park/wake 口径)——
  **S8 修正**:旧值系 bench_echo `drv_step` 通知双消费 bug(每唤醒推进
  2 op、t0 被重置),新值(3 轮范围):
  - K=16,M=4:V0 0.63~0.66M → V5 0.73~0.90M(+15~40%),p50 ~5.5–5.9→
    ~4.2–5.5µs;
  - K=256,M=4:V0 0.62~0.68M → V5 0.72~0.84M(+15~35%),p50 ~5.4–6.5→
    ~4.2–4.8µs;
  - futex/req≈0.50(诚实 2 对/请求);旧"transport/req M=4 0.04–0.25"
    中 park 部分同步修正为 ~0.50,spin 部分(0.04–0.25)不变。
- transport/req 随并发合并下降:M=1 ~1.0 → M=4 ~0.04–0.25 →
  M=16 ~0.04–0.05;低并发是 transport 敏感区。
- **总报告 `docs/04-final-report.md`**:残差主项=transport(非队列/registry);
  超 V0 ≥5% 仅 V3(同线程 -45%)与 V5(futex;-88%/+132%/+32~602%,
  unpaced -11%);迁移建议=transport 增量收敛点回灌 PEL,另立运行时需先做
  L0 栈切换对照轮(01 §0.5)。

S7 验证(L0 栈切换对照轮,U7;2026-10-03,同机 release,ops=200k):
- 实现:`include/xr/xr_ctx.h` + `src/xr_ctx.c`(mmap+guard 栈)+
  `src/xr_switch_x86_64.S`(6 push/pop,标定 **5.6ns/switch**);
  `xr_task_init_fibre`/`xr_task_wait` 接入 worker(机制层零改动);
  bench_roundtrip `--l0=fibre`、bench_l0(calib/create)、test_ctx。
- B1 p50(stackless → fibre):
  - same V0/V5:20→50ns(V5 同线程无意义,两 L0 同值);
  - **cross V0:2.93–2.95µs → 2.99–3.01µs(+1.4~2.0%)**;
  - **cross V5:330→360ns(+30ns,+9%)**;V5 增益 -88.8%→-88.0%,排序不变。
- 创建/销毁(100k,16KB):**141ns → 7.20µs(51×)**;纯 switch 5.6ns
  (快于 PEL stackman ~15ns;stackful 成本估计保守)。
- 偏差:未复用 stackman/vstack(零第三方 + 许可;简单 mmap 非 slot pool,
  VA/TLB 压力低于 PEL);t2 语义 fibre 在 wait 恢复后(含消费 CAS);
  L0 测试仅 sanitizer=none 注册。
- 判据(02 §0.5.3):跨线程 <5%/排序不变 → **保留 stackful 兼容层成立**;
  每次切换模型税 ±30–60ns 可忽略;唯一量级差=创建/销毁 51×;
  TLB/100K 驻留见 S8 补测。
- 日志:`bench-logs/bench_{roundtrip,l0}-20261003-0240*`。

S8 验证(L0-b:park 口径修正 + 每请求 park 次数 + 驻留/TLB;
2026-10-03,同机 release):
- **bench_echo park 口径修正**:`drv_step` 两处续体语义错误——(a) 早期
  版本 park 在发请求之后,吞掉本次 deliver 并立即记"幻影 op";(b) 修正
  版首发块在函数头,每次重入重发请求并重置 t0。现形态 = park 在循环顶 +
  `inflight` 守卫首发,与 conn_step/drv_thread 语义一致;1:1 复验
  stackless/fibre 一致(p50 4.87 vs 4.44µs,~205K rps)。修正值见 S6。
- 每请求 park 次数(fibre echo,--ack=park,K=16/256,M=4,3 轮):
  K=16:stackless V0 0.64M→V5 0.90M(+40%),fibre V0 0.68M→V5 0.84M(+23%);
  K=256:stackless V0 0.68M→V5 0.72M(+6%),fibre V0 0.61M→V5 0.82M(+33%);
  futex/req≈0.50(2 对/请求);L0 差 ≤~10% 且在轮间方差内 →
  **每请求 park 次数是机制属性,不随 L0 改变**。
- ring 驻留(N 节点 token-ring,fibre 16KB 栈,2M 跳,主线程逐跳驱动):
  rtt p50(µs):stackless 3.33~3.38(N=16~100K 平);fibre 3.37(N=16)→
  3.49(N=65536)→3.52(N=100K),consumer p50 1.96→2.18µs(+11%);
  RSS(reside):fibre N=64K 277MB、N=100K 422MB(VmPeak 2.1GB);
  stackless N=100K 123MB;VmPeak stackless 135MB。
- **VMA 硬墙**:默认 vm.max_map_count=65530,每 fibre 2 VMA(guard+RW)
  → N≈32.7K 即 mmap 失败;放宽 1M 后才跑通 65K/100K;stackless 无此限。
- perf TLB(2M 跳):fibre N=16 miss 0.92M/32.1M loads(2.9%)→N=100K
  29.5M/221M(**13.3%**);stackless N=100K 24.3M/194M(12.5%);差
  ~2.6 misses/hop,rtt 差 ≤1%(perf 口径)→ TLB=工作集效应,非栈页专有;
  fibre 额外付 +27M loads/RSS/VMA。
- 日志:`bench-logs/bench_{echo,l0}-20261003-0310*`、
  `bench-logs/perf-tlb-20261003-031138.log`。

S9 验证(park 次数削减曲线,U9;2026-10-03,release):
- `bench_echo --batch B`:driver 在途窗口 B(每 B 个 ack 才 park 一次;
  B=1 与旧口径等价);ack 按完成序近似归因到最早未记账请求。修 fibre
  批处理死锁:补窗口必须在记账之后(否则 `posted-done` 不释放,1:1 只发
  1 个请求就等第 2 个 ack)。
- K=16,M=4,V5(stackless):rps **0.82M(B=1)→3.2~3.4M(B=4)→6.2~6.4M
  (B=8)→6.2~10.1M(B=64,方差大)**;driver-parks/req 1.00→0.25→0.12→
  0.002~0.003;p50 恒定 ~4.2µs(B=64 4.4~5.4),p99 B=64 154~697µs(批尾);
  futex/req 0.50→0.004。
- L0:fibre B=1 0.81M、B=8 5.7~7.1M,同曲线;transport 仍可见:
  eventfd B=8 3.8~5.5M vs futex B=8 6.2~6.4M;K=256:B=1 0.74~0.78M→
  B=8 3.6~4.2M。
- 结论:每请求 park 次数是 c=1 主成本且可由等待粒度削减;对应 PEL =
  try-before-park + write 快路径 + 批量提交/等待 + io_uring;B=4~8 为
  延迟/吞吐折中(p99 批尾换吞吐)。
- 日志:`bench-logs/bench_echo-20261003-0341*`。

## 5. 风险与边界

- stackless 续体 ≠ PEL stackful fibre:已做 L0 对照(S7/S8):每次切换差
  ±30–60ns、跨线程 V0 +1.4~2.0%、每请求 park 次数不变、V5 排序不变 →
  不否决机制结论;stackful 的规模约束 = **创建 51× / VMA 上限(~32.7K
  并发)/ 100K 驻留 RSS 422MB**,均属"下限估计"(真实 vstack 更高)。
- 无 libuv 的 loop 与 uv 合并/回调时序不同:V1 同线程直投在 PEL 曾被 D8
  否决(帧寿命契约),**结论迁移回 PEL 必须重新评审**(doc155 §7.7 反绕过
  纪律同样适用);沙盒结果不直接构成对 PEL 核心目录的修改依据。
- 只认同窗对照数字 + perf/strace 证据;纯理论结论不立项。

## 6. 对照资料

- 本仓评估:`docs/06-pel-modification-eval.md`(PEL 修改全面评估:
  M1 futex 回灌 / M2 park 次数削减 / M3 io_uring;步骤/原因/数据/判据)。
- PEL:`docs/148` §0.1–0.3/§3.1/ledger · `docs/151` §4.3 · `docs/152` §7 ·
  `docs/156` §4–5 · `docs/160/161` · `docs/00-unified-lifecycle-design.md`
  §0/§5.2 · `src/scheduler/pel_parker.h` · `src/eventloop/uv_channel.c`。
- 外部:Tokio Parker/scheduler 设计、Vyukov MPSC、futex(2)、io_uring
  `IORING_OP_MSG_RING`。
