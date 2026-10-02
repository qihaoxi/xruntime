# xruntime 02 — 上限路径设计:parker / io wakeup 的极限形态

日期:2026-10-02 · 状态:设计(待评审) · 定位:**上限路径**,与 01-plan 的 PEL 对照沙盒并行

## 0. 定位与关系

- `01-plan.md`:PEL 对照沙盒——V0 复刻 PEL 形态,单变量 A/B,结论面向"能否回灌 PEL";
- 本文:**上限路径**——不背 PEL 契约(弱句柄 miss-drop / 发布权 claim / 三态
  payload),以 Tokio / Project Loom / HotSpot / Go 为参照系,定义唤醒链每段的
  **物理下限**,给出 V0→极限的完整形态、分段预算与验收判据,回答:
  **"唤醒链还能压到多低?另立高上限运行时是否成立?"**
- 变体编号与 01-plan 的 V0–V5 不冲突:V0–V5 语义不变,本文为每个变体标注
  **上限形态**并追加 V6–V9(超出原矩阵的极限手段);
- 结论边界:上限路径的结论**只用于"另立运行时"决策**,不直接回灌 PEL
  (PEL doc155 §7.7 反绕过纪律同样适用,回灌须按 PEL 00 §0.3.2 重新评审)。

核心命题:**把"任务就绪"与"线程睡眠"两层解耦**(Tokio 形态)——任务唤醒走
队列(同线程零 syscall),线程睡眠只在队列空时发生,transport 仅在目标确已睡眠
时付出。V0 把两层揉在 eventfd 一条链上,是当前每对 park/wake ~3µs 的结构性来源。

## 0.1 首要原则:统一收口(单一漏斗)

本仓约束唯一正文为 `docs/00-unified-constraints.md`:其 §0 规定"同一功能只允许
一个权威收口点,所有调用路径必经之;AOP/横向切面只在收口挂接",并给出收口点
清单与守卫。**本文所有变体设计必须遵守该文,不在此重复**。

## 0.5 整体架构与 L0 栈切换选型

### 0.5.1 分层(栈切换与机制层正交)

```
L4 编程模型     blocking(@await) │ async/状态机           ← 决定 L0 要不要栈
L3 调度器       LIFO slot + local queue + inject + 批 drain (V1/V3/V7/V8)
L2 唤醒原语     parker 单字 permit(V6) + 内嵌 waker(V4) + registry/句柄
L1 传输/事件面   futex(V5) │ eventfd+epoll │ io_uring(V9)
L0 栈切换       stackless(函数调用) │ stackman+vstack │ 自研最小 asm
```

**正交原则**:L1–L3 的机制变体(V0–V9)不依赖 L0 的具体形态。同一套 parker/
队列/transport 应在两种 L0 下各跑一遍,才能把**机制税**与**栈模型税**分解开
——这是"另立运行时"决策唯一可靠的口径。

### 0.5.2 L0 候选事实(PEL doc22/23 实测基线)

| 维度 | stackless | stackman + vstack |
|---|---|---|
| 切换原语 | 函数调用 ~2ns | 6 push + SP 交换 + 6 pop ≈ **15ns**(接近 asm 下限) |
| 切换总计(含状态管理) | ~42ns | ~100–200ns(余量在 sched lock/状态管理,非 switch) |
| 创建 / 销毁 | ~200–500ns / ~100–200ns | ~5–10µs / ~2–5µs(vstack mmap/madvise) |
| 内存 | state ~数百 B | `fibre_stack_t` + vstack 活页 ≈ 8.4KB/fibre |
| TLB | 近零 | 每 vstack ≥2 条目;100K fibre ≈ 200K 条目 → 抖动 |
| 编程模型 | 需状态机/编译期改写 | 保留阻塞 `@await` 语义 |
| 工程面 | 新路径/取消/scope 竞态需重建 | PEL 已有 ASan 集成、死区回收、slot pool 经验 |

- **stackman 不是瓶颈**:15ns 已近下限,自研 asm 收益 <10ns,第一阶段不做;
- **真正的模型税在 vstack 副作用**:8MB VA/fibre、TLB 抖动、创建销毁 µs 级、
  VMA 压力、活区不可回收(doc160 零页丢唤醒红线);
- **stackless 的代价是编程模型**:PEL doc23 判定"产品无 100K+ 需求,不切换";
  xruntime 上限路径正是用同窗数据回答该判定是否仍然成立。

### 0.5.3 选型路径(两阶段对照 + 判据)

1. **第一阶段(现状)stackless**:机制天花板的最干净测量环境,Tokio 对位;
2. **第二阶段 stackful 对照轮**:复用现成 `stackman+vstack`(不自研),同一组
   V1–V9 再跑一遍;新增 L0 维度但机制代码零改动;
3. **判据**:
   - 两 L0 差距 <5% 且不改变变体排序 → **保留 stackful**(兼容 PEL 阻塞生态,
     机制结论可回灌);
   - 100K+ 并发 / 高频切换下差距显著(TLB + 创建销毁 + 切换吞吐)→ **另立
     运行时以 stackless 为主、stackful 作兼容层**;
4. **接口要求**:L0 以 `xr_ctx_switch` 形态抽象(resume/suspend 两个原语 +
   task 状态),stackless 与 stackman 各做一个实现;禁止机制层直接 include
   stackman。

### 0.5.5 第一阶段结果(U7,2026-10-03)

> **仪器偏差(相对 PEL)**:沙盒零第三方,未复用 stackman/vstack——stackful
> 后端为 `src/xr_ctx.c` + `src/xr_switch_x86_64.S` 自研最小 6 push/pop
> (**5.6ns/switch**,快于 PEL stackman ~15ns,对 stackful 有利,结论保守);
> 栈用 mmap+guard 页(非 vstack slot pool/madvise),VA/TLB 压力低于 PEL
> 8MB VA/fibre,创建销毁 7.2µs 与 PEL doc22 5–10µs 同量级;L0 测试仅在
> sanitizer=none 门禁注册(自定义栈与 ASan/TSan 不兼容)。

| B1(release,ops=200k,3 轮中位) | stackless p50 | fibre p50 | Δ | V5 增益 |
|---|---|---|---|---|
| same-thread V0 | 20ns | 50ns | +30ns | — |
| same-thread V5 | 20ns | 50ns | +30ns | — |
| cross-thread V0 | 2.93–2.95µs | 2.99–3.01µs | **+1.4~2.0%** | — |
| cross-thread V5 | 330ns | 360ns | +30ns(+9%) | stackless −88.8% / fibre −88.0% |

- **创建/销毁(100k,16KB 栈)**:stackless 141ns/个 vs fibre 7.20µs/个
  (**51×**,mmap/mprotect/munmap 主导);
- **对照判据(§0.5.3)**:
  - 跨线程 V0 差距 <5% ✅;V5 快路径 +9%(绝对 +30ns/对,约为 PEL 实测
    每对 ~2.9µs 的 1%);同线程绝对 +30ns(微口径放大);
  - **变体排序不变**(V5 V0→−88% 量级;unpark 分布不变);
  - 高频切换吞吐无差异(纯 switch 5.6ns);**创建/销毁 51× 是唯一量级差**;
    TLB/100K 驻留未测(需 perf,见 05);
- **结论**:机制结论与变体排序不随 L0 改变 → "保留 stackful 作 PEL 兼容层"
  的判据成立,机制回灌不被栈模型否决;每次切换的模型税可忽略(±30–60ns),
  PEL 对 rust 的 0.37–0.69× 不能由切换成本解释,更可能是阻塞式 API 的
  **每请求 park 次数**与事件面形态;若目标形态含高频任务创建(per-request),
  创建/销毁 51× 与未测的 TLB 需先补证再决定另立运行时。
- **驻留/TLB/VMA 补测(S8,N 节点 token-ring,2M 跳)**:
  - rtt p50(µs):stackless 3.33~3.38(N=16~100K 平);fibre 3.37(N=16)→
    3.52(N=100K),consumer p50 1.96→2.18µs(+11%);
  - perf TLB:fibre N=16 miss 2.9%(0.92M/32.1M loads)→N=100K 13.3%
    (29.5M/221M);stackless N=100K 12.5%(24.3M/194M)——差 ~2.6/hop,
    说明 TLB 压力主要是**工作集效应**,非栈页专有;rtt 差 ≤1%(perf 口径);
  - **VMA 硬墙**:vm.max_map_count 默认 65530,每 fibre 2 VMA →
    N≈32.7K 并发即 mmap 失败(放宽到 1M 后才到 100K);stackless 无此限;
  - RSS:N=100K 驻留 fibre 422MB(VmPeak 2.1GB)vs stackless 123MB。
  - **判据更新**:高频创建/销毁 51× 与 VMA/RSS 一起构成 stackful 的规模
    约束;TLB 不再是首要疑虑。二者由 vstack slot pool/复用摊薄,但需在
    PEL 同窗实测,沙盒数为下限。

## 1. 上限的定义:分段成本模型与物理下限

### 1.1 唤醒链分段

```
park 侧:    park 判定 ─────────────────────────────┐
unpark 侧:  仲裁/发布 → 句柄解析 → 入队 → transport → loop tick → resume → step
```

- **park 判定**:等待者把状态置 PARKED 并让出执行权;
- **仲裁/发布**:多生产者竞争"本次唤醒"的发布权并写入 payload;
- **句柄解析**:从唤醒句柄(弱句柄/强句柄)找到目标任务;
- **入队**:就绪任务进入调度队列;
- **transport**:打断目标线程睡眠(或同线程直接省掉);
- **loop tick**:事件面返回、批量取事件、调度循环推进;
- **resume**:从挂起点恢复执行(stackless=函数调用,stackful=栈切换);
- **step**:业务本身(不在本仓优化面内)。

### 1.2 单段物理下限(本机 16 核,Ryzen,tsc_hz≈3.79GHz,来自 bench_env)

| 段 | 操作形态 | 物理下限(量级) |
|---|---|---|
| 原子 RMW(同核无争用) | CAS / fetch_add / swap | ~5–20ns |
| 原子 RMW(跨核,含 cache line 转移) | 同上 | ~40–100ns |
| 函数调用/间接调用 | stackless resume | ~2–5ns |
| 栈切换(stackful 对照) | PEL doc22 预算 | ~20–100ns |
| futex_wait / futex_wake | 1 次 syscall | ~0.3–1µs |
| eventfd write + epoll 唤醒 | 2 次 syscall(写+等) | ~1–2µs |
| 线程上下文切换 | 调度器介入 | ~1–3µs |

**上限形态的目标**:同线程 handoff = 2 个 RMW + 入/出队 + 调用(几十 ns);
跨线程且目标未睡 = 1 次 cache line 转移 + 入队(百 ns 级);跨线程且目标已睡
= 上述 + 1 次 futex_wake(µs 级);高并发批合并后 transport/op → 0。

### 1.3 三态+claim vs 单字 permit(本仓最大的一段差)

| 维度 | V0/PEL:三态 + 发布权 claim | Tokio/rust std futex parker | LockSupport permit |
|---|---|---|---|
| 状态存储 | `state` + `pub` + `payload` | **1 个原子字** | 每线程 1 个 permit 位 |
| park 快路径 | 1 CAS | 1 `fetch_sub` | 1 CAS |
| unpark 快路径 | claim CAS(可自旋)+ payload store + door CAS | **1 `swap`** | 1 CAS |
| 未睡目标 transport | V0 恒 eventfd(V2 门控) | `swap` 返回值判定,0 syscall | 0 |
| 已睡目标 transport | eventfd write | `futex_wake` | futex/condvar |
| payload/reason | 内建(发布权保证完整) | 无(上层外置字段 + 重查) | 无(同上) |
| 多生产者合并 | claim 仲裁(first-wins) | `swap` 天然 last-wins 合并 | permit 合并 |
| 丢唤醒防护 | 三态 | permit(两态编码进单字) | permit |

结论:V0 每对 park/wake 需 ~4–6 个原子操作 + 锁 + 查找 + transport;上限形态
是 **2 个 RMW + 0/1 次 syscall**。这是 parker 段的理论天花板,也是 V6 的依据。

## 2. 参照系拆解

### 2.1 Tokio parker(`runtime/park.rs`,unstable futex 版)

- 单原子字三态:`EMPTY=0 / NOTIFIED=1 / PARKED=MAX`;
- `park`:先 `fetch_sub` 消费通知,未命中则 `futex_wait(state==PARKED)`;
- `unpark`:`swap(NOTIFIED)`,**仅当返回 PARKED 才 `futex_wake`**——未睡目标零
  syscall,多生产者天然合并;
- 稳定版为 `AtomicUsize + Mutex + Condvar`,另有 `CURRENT_PARKER` thread-local
  与 `UnparkThread`(Arc 引用计数句柄);
- 迁移形态:**V6 单字 permit parker + V5 futex transport**;`UnparkThread` 对应
  V4 的引用计数唤醒句柄。

### 2.2 Tokio 调度器(`scheduler/multi_thread`)

- **LIFO slot**:任务唤醒另一个任务时优先放 worker 的 LIFO 槽,当前任务结束后
  立即跑它(缓存局部性);连续使用上限 `MAX_LIFO_POLLS_PER_TICK=3` 防饿死;
- **local run queue**:容量 256 的 SPMC 队列,溢出时一半搬去 inject;steal 取一半;
- **inject queue**:跨线程唤醒/非 worker 线程提交走全局 MPSC;
- 节拍:`event_interval=61`(每 61 次任务 poll 一次 I/O),`global_queue_interval=31`
  (每 31 次本地取一次全局);
- 迁移形态:**V1/V7 安全直投 + V3 本地队列 + V8 批事件**。

### 2.3 Tokio I/O driver(`runtime/io`)

- `ScheduledIo` **cache line 对齐**(`repr(align(64))`),状态字打包
  `{shutdown | tick | readiness}` 于一个 `AtomicUsize`;
- waiters 为 `Mutex<LinkedList<Waiter>> + reader/writer` 两个 Waker 槽;
- `wake()`:锁内收集 waker 到栈上 `WakeList`(≤32),**锁外唤醒**(延迟唤醒,
  与 PEL P3 同型);`tick` 用于边沿触发 readiness 的失效判定;
- 每个 driver 一个 `mio::Waker`(Linux=eventfd),仅在需要打断 epoll 时写;
- 迁移形态:**V8 事件批 + V3/V4 唤醒收集与投递分离**。

### 2.4 Project Loom(`VirtualThread` / `Continuation`)

- VT 状态机:`PARKING/PARKED/PINNED/TIMED_PARKED/UNPARKED/YIELDING…`,park 即
  `Continuation.yield`(卸载 carrier),unpark 即 CAS `PARKED→UNPARKED` 后把
  continuation 任务重新提交给 ForkJoinPool;
- 网络阻塞:`NioSocketImpl.park` 向 Poller(专用 epoll 线程)注册 fd,再 park VT;
- permit 语义:`LockSupport` 至多一个 permit、允许虚假唤醒、调用方重查条件;
- 迁移形态:**"注册 + 挂起"与"就绪 + 重新入队"分离**(xruntime 中即
  parker 状态翻转 + 调度队列,而非在 unpark 内直跑)。

### 2.5 HotSpot(`LockSupport` / `ObjectMonitor`)

- `LockSupport.park/unpark`:per-thread permit(至多 1),park 可虚假返回,
  必须循环重查;Linux 下慢路径 futex/condvar;
- `ObjectMonitor`:瘦锁=mark word 上 CAS;失败先自适应自旋(指数退避)再膨胀;
  膨胀后 `_owner` CAS + `_cxq`(CAS LIFO 新到达)+ `_EntryList` + `_succ`
  (heir presumptive),**competitive handoff**(后继者醒来重新竞争,不直接移交),
  且一次只 unpark 一个后继(futile wakeup 节流);
- 迁移形态:**快路径单 CAS + 失败自旋 + 慢路径才付系统调用**的通用阶梯;
  `_succ` 对应"门控唤醒只在确有睡眠者时发生"。

### 2.6 Go netpoller(交叉验证)

- `pollDesc` 的 rg/wg 是**二值信号量**状态机(`pdReady/pdWait/G 指针`),
  netpoll 返回就绪 `gList` 批量注入 P 的 runq;
- P 有 256 槽 runq + `runnext`(LIFO 槽,与 Tokio 同构);`wakep` 用
  `nmspinning` 抑制无谓线程唤醒;
- `findrunnable` 顺序:本地 runq → 全局 → netpoll → steal,与 Tokio 节拍同族。

### 2.7 参照系对位表

| 机制段 | Tokio | Loom | HotSpot | Go | xruntime 上限变体 |
|---|---|---|---|---|---|
| 任务 park | 单字 futex parker | VT 状态 + permit | permit(单线程) | pollDesc 信号量 | V6 |
| 唤醒句柄 | `UnparkThread`(Arc) | Poller 持 VT 引用 | `JavaThread*` 直达 | `g*` 直达 | V4 |
| 就绪队列 | local(SPMC)+inject+LIFO | ForkJoinPool | EntryList/_cxq | runq+runnext | V3/V7 |
| I/O readiness | `ScheduledIo`(对齐+打包) | Poller(epoll) | —(阻塞式) | netpoll(epoll) | V8 |
| 事件面唤醒 | `mio::Waker`(eventfd) | poller 线程 | futex | netpollBreak(eventfd) | V5 |
| 批/节拍 | event_interval 61 | FJP steal | 自旋+节流 | findrunnable 顺序 | V8 |

## 3. 变体到极限形态的映射

> V0–V5 语义按 01-plan 不变;下表是"同一变体做到底"的形态;V6–V9 为追加。

### V1 同线程安全点直投 → 上限形态:tokio LIFO slot

- deliver 只把任务放入 owner worker 的 **LIFO 槽/本地队列**,同线程时
  **transport=0**;worker 在 step 边界(当前任务返回后)取槽执行;
- **禁止在 deliver 回调内 resume 任务**(PEL D8 教训:唤醒发起方栈帧寿命——
  unpark 之后调用方仍会触碰 op/timer/channel;直跑等于让渡整条调用栈);
  xruntime 虽为 stackless(无栈切换),该契约仍成立:deliver 回调栈帧内不得
  重入 worker 循环/parker pub;
- 上限:同线程唤醒 = 2 RMW + 1 指针入槽 + step 边界取出调用;LIFO 连续
  3 次后强制走普通队列(公平性兜底)。

### V2 parked-flag 门控 → 上限形态:parker 级精确门控(并入 V6)

- 上限不是 worker 级 `sleeping` 原子,而是**状态字本身携带 parked 位**:
  `unpark` 的 `swap` 返回值即判定是否需要 transport,零额外原子、零误唤醒;
- worker 级门控保留为"事件面写合并"的补充(V2 现状),但两者不可混淆。

### V3 lock-free ready 队列 → 上限形态:per-worker SPMC ring + inject

- 本地环:生产者=deliver(跨线程)+ 自身,消费者=owner;容量 256,满时一半
  搬 inject;头尾分离 cache line;
- inject:跨线程/外部提交的 MPSC(Vyukov 有界环或 crossbeam Inject 形态);
- 单 worker 沙盒下先落"SPMC 本地环 + 溢出链",work stealing 不在第一阶段。

### V4 waker lifetime → 上限形态:intrusive 唤醒句柄(无 registry)

- task 内嵌唤醒句柄(worker 指针 + task 指针/世代号),deliver 直达,无 hash
  查找、无 mutex;
- 生命周期两选一:① 引用计数 waker(Tokio `Arc` 形态);② owner-guaranteed
  (task 摘除前禁止外部 parker 触碰,由活跃计数/世代号防护);选型以
  B1 解析段实测为准;
- PEL 弱句柄 miss-drop 是保守解,上限路径不需要它——这是与 PEL 的**故意分叉**。

### V5 transport 备选 → 上限形态:futex 直达,eventfd 仅作事件面备选

- worker 睡眠原语改 **private futex**(单原子字,`futex_wait/wake`);
- eventfd/epoll 只保留给"真实 IO readiness"(需要 poll 多个 fd 时);
- 批量:一次 `futex_wake` 唤醒多个 waiter、或 `io_uring MSG_RING`(内核支持
  探测,记录不默认启用);
- 判据:`wake_writes/op` 在 fan-in 下趋 0,已睡目标每次唤醒恰 1 次 syscall。

### V6 单字 permit parker(追加)

- 把 `state + pub + payload` 压成**一个原子字**(`EMPTY/PARKED/NOTIFIED`)+
  外置 payload/reason 字段;`park` 消费 CAS,`unpark` `swap` 并按返回值决定
  transport;多生产者 last-wins 合并;
- 代价(必须显式接受):无发布权仲裁、无内建 reason、允许"合并丢事件"——
  需要上层重查循环 + 取消/超时走独立字段(对照 LockSupport 契约);
- 这是 parker 段的理论天花板形态,作为 V1/V2/V5 的公共底座。

### V7 LIFO slot 调度(追加)

- 同 worker 唤醒优先直跑下一任务(限 3 次),降低 cache 抖动与唤醒延迟;
- 与 V1 的区别:V1 是"省 transport 的直投",V7 是"调度选择策略";两者叠加。

### V8 批事件/批 drain(追加)

- `epoll_wait` 一次取多事件;drain 至空再回 poll;节拍化(每 N 次任务 poll 一次,
  对齐 Tokio `event_interval`),把 loop 往返摊薄到批;
- fan-in 场景:唤醒合并(latch)+ 批量入队 + 单次 transport,目标
  `transport/op → 0`(V0 无节流时已实测 99.7% merged,上限形态把该形态做成
  默认而非特例)。

### V9 内核面可选(追加,记录不默认)

- `io_uring` `IORING_OP_MSG_RING`(跨线程 ring 唤醒)/ futex2 / `eventfd_signal`
  内核侧合并;内核版本探测 + 单独记录,不进默认路径。

### V9.1 transport 后端与启动期能力探测(跨平台对位)

**前提(硬性)**:`IORING_OP_MSG_RING` 的目标是**另一个 io_uring 环**,不是
epoll loop。loop 若仍睡在 `epoll_wait`,MSG_RING 叫不醒它——要用它当 transport,
loop 必须整体睡在 `io_uring_enter`(IO 也走 `IORING_OP_POLL_ADD/READ/WRITE`)。
**MSG_RING 是"全面 io_uring 运行时"的原生 wake,不是 epoll loop 的 drop-in**;
epoll 仍作 IO 面时,futex hybrid 更简单且已被 S5 实测(RTT 390ns)。

**Linux 启动探测阶梯(能力探测,不是版本号判断)**:

```c
/* 1) 建环:被 seccomp 拦(EPERM/EACCES)/sysctl 禁(ENOSYS/EPERM)立即降级 */
if (io_uring_setup(entries, &p) < 0) goto fallback_futex;
/* 2) 能力位:5.6+ IORING_REGISTER_PROBE(IO_URING_OP_SUPPORTED);
 *    6.15+ IORING_REGISTER_QUERY 可无环查询(fd=-1),更干净 */
if (!op_supported(IORING_OP_MSG_RING)) goto fallback_futex;
/* 3) 功能探测:建第二环,提交一个 MSG_RING,确认目标环收到 CQE(比位图可信) */
/* 4) 全部通过:transport_backend=io_uring_msg_ring;记录到启动日志/统计 */
```

现实约束(必须当主路径处理):Docker **默认 seccomp 已封 io_uring**、
`kernel.io_uring_disabled=2` 可全系统禁、Google 曾整舰队禁用;探测失败→回退
不是异常。回退阶梯:`MSG_RING → futex hybrid → eventfd+epoll`。

**跨平台对位**:

| 平台 | loop 唤醒(可 poll) | 线程睡眠原语 | 备注 |
|---|---|---|---|
| Linux | eventfd+epoll / io_uring MSG_RING | futex | 需 hybrid 或全 io_uring |
| Windows | **IOCP `PostQueuedCompletionStatus`**(XP+) | 同左 | **单一等待覆盖 IO 完成+自投递**,无需 hybrid;批量 `GetQueuedCompletionStatusEx` |
| Windows 11 | **IoRing**(Build 22000+):`CreateIoRing`/`QueryIoRingCapabilities`/`IsIoRingOpSupported` | 同左 | io_uring 对位,官方提供能力探测 API |
| macOS/BSD | `kqueue` `EVFILT_USER`/`NOTE_TRIGGER` | `os_sync_wait_on_address`/`os_sync_wake_by_address_any`(macOS 13+/iOS 16+,公开;更老私有 `__ulock_wait/wake`) | kevent 也叫不醒 os_sync,同需 hybrid |
| macOS 原生 | mach port(`EVFILT_MACHPORT`) | `mach_msg` | libdispatch/GCD 的跨线程唤醒底座 |

**接口收口(硬性)**:transport 是唯一漏斗(constraints §0)——`probe()` /
`wake()` / `wait(timeout)` / `capabilities` 四个原语,每后端一个实现;探测结果
(`transport_backend=...`)必须进启动日志与基准报告,A/B 时后端是显式变量。
**fallback 必须进 CI 矩阵**(老内核/开 seccomp 容器),否则等于没测。

**本仓范围**:沙盒按 01-plan 只做 Linux(不做跨平台兼容层);本轮只落
`probe` 的 Linux 阶梯与记录(V9),Windows/macOS 后端属"另立运行时"立项范畴。

### V9.2 睡眠点复用 vs 角色分层(notify / transport / resume 三分)

**物理约束**:一个线程一次只能阻塞在一个睡眠点;futex 叫不醒 `epoll_wait`。

**三分模型(概念上本来是三件事)**:

| 层 | 语义 | 拥有者 | 可否合并 |
|---|---|---|---|
| **notify(任务就绪)** | parker 状态 + payload + 入队 | 唤醒方 | 可合并(多次 notify→一次唤醒) |
| **transport(线程唤醒)** | 仅目标线程确在睡时,按其睡眠原语打断 | 唤醒方 | 可合并;醒着=零成本 |
| **resume(执行恢复)** | owner drain 后真正跑(stackless=调用/stackful=栈切换) | 消费者 | 批量化、LIFO/公平策略 |

**PEL 现状**:`pel_scheduler_wakeup` 恒 `uv_async_send`,把 notify+schedule+
transport 融合在唤醒方一次调用(transport 恒发,靠 libuv pending 合并);
resume 已延迟到 check/drain,但 `fibre_registry_resume_handle` 的命名把
"schedule+notify"叫成了 resume。**收益证据**:B2 `eventfd/req` 0.995→0.124→
0.019(N 次 notify 合并为 1 次 transport);B1 同线程 50ns vs 跨线程 3µs
(醒着不 transport);V5 RTT -88%(transport 只在确已睡时发生)。

**判据(不是"有无 IO",而是"目标线程睡在哪")**:
- 目标只做纯任务交接 → 睡 futex → 任务唤醒走 futex;
- 目标要等 fd → 睡 epoll → 打断必须走 eventfd / io_uring;
- 两类混在同一线程时只能 hybrid(按当前等待对象路由),并处理"决定睡
  futex vs epoll"的竞态窗口。

**tokio 式角色分层(上限路径目标形态)**:

```
worker 线程:无活 → 睡 futex(V5 单字);任务唤醒 = 入队 + futex unpark
活跃线程  :持 I/O driver → 睡 epoll;IO 就绪 → 转成任务唤醒(入队+unpark)
eventfd   :仅 poll 集变化/超时/关闭等控制面(低频)
```

- 高频"任务就绪"与低频"内核等待集变化"分道,歧义消失;stackless 下 IO 完成
  只需 re-poll,不需要跨线程 resume(每 IO 一跳的代价不存在);
- io_uring 可用时进一步统一:loop 睡 `io_uring_enter`,CQE + MSG_RING 同源
  (V9.1),eventfd 也可省;
- 收敛点不变:notify=`task_deliver` · transport=`worker_wake`(唯一) ·
  resume=`worker_run_task`;三者除明确的安全点直投(V1/V7)外不得互相内联。

## 4. 分段预算与目标数字

V0 实测(01-plan §4 S2,3 轮 median):B1 跨线程 RTT=3.0µs(producer_side
1.24µs / consumer_wake 1.75µs,p99 4.7µs);B2 `--wait` M=1 eventfd/req=0.995。

| 变体 | 压制的段 | V0 形态 | 上限形态 | 设计目标(待 B1/B4 验证) |
|---|---|---|---|---|
| V1/V7 | transport+调度 | 恒 eventfd+check | 同线程零 syscall+LIFO | 同线程 RTT < 500ns |
| V2/V6 | 仲裁+发布 | claim 自旋+payload+door CAS | 1 `swap` | unpark 段 < 50ns |
| V3 | 入队 | mutex 侵入链表 | 2 RMW SPMC | 入队 < 30ns |
| V4 | 解析 | mutex+1024 桶 hash | 0 查找(内嵌) | 解析段 ≈ 0 |
| V5 | transport | eventfd 恒发 | futex 按需 | 未睡 0 / 已睡 1 syscall |
| V8 | loop tick | 每事件一轮 | 批量摊薄 | transport/op → 0 |
| 综合 | — | 跨线程 RTT 3.0µs | 2 RMW+1 cache line+按需 syscall | 跨线程 RTT ≤ 1.5µs |

**上限参照**:终局目标不是绝对值,而是**同机同窗的 tokio 形态对照**(见 §5 B6);
若最优变体达到同机 tokio 的 ≥0.9× 且无 regress,则"另立运行时"成立;否则
PEL 模型税的定性维持。

## 5. 基准扩展

- **B1 已有**:`--same-thread` / `--flags` 已由 S3 会话加入,直接跑
  V0/V1/V2 同窗矩阵(同线程/跨线程分组,producer_side/consumer_wake 分段);
- **B4(新)parker 微基准**:纯 park/unpark 对,不经 worker/loop,直接对比
  三态+claim / 单字 permit / futex 三形态,测出 §1.2 各段下限——"理论上限"的
  最直接证据;
- **B5(新)transport 批形态**:fan-in M→1,画 `wake_writes/op` 随 M 的曲线
  (V2/V6/V8 应趋 0);
- **B6(可选)同机 tokio 对照件**:独立子目录、不链接进 xr(纯基准参照);若
  不引入 Rust 依赖,则引用 PEL doc151 §4.3 同机记录并标注跨窗不可比;
- 分段计时补齐:现有 claim/publish/deliver/registry/队列/eventfd,新增 park
  段与 resume 段(tsc 打点);
- 纪律不变:env-check → warmup → ≥3s 测量 → 3 轮 median → 同窗单变量 A/B →
  超出轮间噪声(≥5% 或分布前移)才保留,否则回退留档。

## 6. 风险与边界

1. **契约代价**:permit 模型无 payload/reason、无发布权,允许合并/虚假唤醒;
   取消、超时、多等待者语义必须由上层重查循环 + 外置字段重建(LockSupport
   契约),这是一次显式的能力交换,不是免费优化;
2. **不可回灌 PEL**:本路径故意去掉弱句柄 miss-drop/claim,结论只用于"另立
   运行时";回灌须按 PEL 00 §0.3.2 与 doc155 §7.7 重新评审;
3. **同线程直投安全点**:deliver 内禁止跑任务/重入 parker pub;LIFO 限 3 次
   防饿死;PEL D8 的"唤醒发起方栈帧寿命"教训在 stackless 下仍需遵守;
4. **多生产者 payload**:V6 last-wins 需上层接受(与 V0 first-wins 语义不同),
   基准的 `--wait` 口径需区分;
5. **stackless ≠ Loom continuation**:resume 成本 ~百 ns 级差异,不改变排序,
   但绝对值上限比 Loom 更低;若吃紧再补栈切换对照;
6. **测量纪律**:上限结论必须同窗对照 + perf/strace 证据,禁止理论数字直接
   入账;跨项目(tokio/rust)数字只作形态参照。

## 7. 执行台账

| 步骤 | 内容 | 状态 |
|---|---|---|
| U0 | 本文设计(参照系 + 分段下限 + V6–V9) | ✅ 2026-10-02 |
| U1 | B4 parker 微基准(三形态同窗) | ⬜ |
| U2 | V6 单字 permit parker(与 V1/V2 底座合流) | ⬜ |
| U3 | V4 内嵌唤醒句柄(去 registry) | ⬜ |
| U4 | V3 SPMC 本地环 + inject | ⬜ |
| U5 | V5 futex transport + V8 批 drain | ⬜ |
| U6 | B1/B2/B5 全矩阵 + B6 参照(可选) + 上限判定 | ⬜ |
| U7 | L0 对照轮:stackman+vstack 实现 `xr_ctx_switch`,机制变体复跑 + 判据 §0.5.3 | ✅ §0.5.5(自研 asm 替代 stackman,偏差已注) |

## 8. 参考

- Tokio:`runtime/park.rs` · `runtime/io/driver.rs` · `runtime/io/scheduled_io.rs` ·
  `scheduler/multi_thread/worker.rs` · `scheduler/multi_thread/queue.rs` ·
  `sync/notify.rs`;mio `waker.rs`(Linux=eventfd)。
- Rust std / tokio unstable:futex Parker(`EMPTY/PARKED/NOTIFIED` 单字)。
- Loom:`VirtualThread.java`(VT 状态机/unpark 重提交) · `Continuation.java` ·
  OpenJDK Loom wiki(park=register+yield,poller=专用 epoll 线程)。
- HotSpot:`runtime/objectMonitor.cpp`(`_cxq`/`_EntryList`/`_succ`/competitive
  handoff) · `basicLock`(瘦锁 CAS+膨胀) · `LockSupport` permit 契约。
- Go:`runtime/netpoll.go`(pollDesc rg/wg 二值信号量) · `runtime/proc.go`
  (runq/runnext/findrunnable/wakep)。
- PEL 基线:doc22(stackman switch ~15ns / 同线程 yield→resume ~100–200ns) ·
  doc23(有栈/无栈切换、创建销毁、TLB 对比与"暂不切换"决策) ·
  doc160/161(vstack 零页丢唤醒红线、1 页活区地板、26× RSS 模型税)。
