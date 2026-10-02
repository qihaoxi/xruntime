# xruntime 00-unified-constraints — 约束总纲(吸收 PEL 00,结合本仓实际)

日期:2026-10-02 · 状态:生效 · 性质:**本仓约束唯一正文**

> 来源:PEL `polyglot-c/docs/00-unified-lifecycle-design.md` v9.0(六机与交互边
> G1–G5、组合元规则 L0–L6、元模式 P1–P16、领域硬性约束、符合性保证)。
> 本仓是**机制沙盒 + 上限路径**,不是产品运行时:只吸收与"唤醒链机制实验"
> 相关的约束,并按本仓实际(task/worker/parker/registry/queue/transport、
> V0–V9、B1–B5)重新落地;**产品域组件(libuv handle/channel/scope/fibre
> 引用计数/ABI 发布面)显式不适用**(§6 登记)。
>
> 关系:`01-plan.md`=计划/判据/台账 · `02-upper-bound-design.md`=上限路径设计 ·
> `03-handoff-s4-s6.md`=实施交接 · **本文件=约束唯一正文**;冲突时以本文件
> 条款 + 同窗实测为准。

## 0. 首要原则:统一收口(单一漏斗,硬性)

> **同一功能只允许一个权威收口点(漏斗);所有调用路径必须经过它,禁止在
> 调用点分散实现/判断。** 这样加 AOP/横向切面(观测、trace、统计、sanitizer
> hook、计费)只在一点挂接;变体替换只改漏斗内部,调用点与上层语义零改动。

**四条推论**:

1. **单点横切**:probe/trace/stats 只允许挂在收口点;禁止散点埋桩——否则每次
   加观测改 N 处,各点口径必然漂移;
2. **单点证明**:状态机、内存序、生命周期证明集中在漏斗内,调用方只持契约
   (PEL G1 触碰点统一 / G2 仲裁点统一 / G5 每域存活证明唯一);
3. **单点变体**:V0→V9 每个变体只替换漏斗内部实现;这是单变量 A/B 成立的前提;
4. **规则数判据**:收口使"必须记住的规则数"净减;散点实现是复杂度峰值来源
   (PEL §0.3.2)。

**本仓收口点清单(新增变体必须沿用,禁止另开旁路)**:

| 功能 | 唯一收口 | 禁止散点 |
|---|---|---|
| park 判定 | `xr_parker_park` | 各 API/适配器自己 CAS state |
| 唤醒/发布 | `xr_parker_unpark` | 各调用点自己写 state/payload |
| 投递入队 | `task_deliver`(V4a 后 `task_deliver_direct` 同语义单点) | 回调里直接 push 队列 |
| transport | `worker_wake`(V5 后为统一 sleep 原语) | 任意位置 `write(eventfd)`/`futex_wake` |
| 任务执行 | `worker_run_task`(step 唯一入口) | 别处直接调 `t->fn` |
| 调度取任务 | `worker_pop`/`next_task`(LIFO/local/inject 策略内聚) | 调用方自行选队列 |
| 统计/观测 | 上述收口内 | 基准/测试各自计数 |

**守卫化(可 grep 静态判定,建议进 CI)**:`write(`+`event_fd` 仅允许出现在
`worker_wake`;`futex(` 仅允许出现在 L0/L1 实现文件;`stat_*` 更新仅在收口
函数;`t->fn(` 仅允许出现在 `worker_run_task`;新增散点即红。

## 1. 状态机与收敛点(吸收 PEL §0.1)

### 1.1 本仓状态机清单

| 机 | 状态 | 转换保护 | 权威入口 |
|---|---|---|---|
| **W worker** | CREATED→RUNNING→STOPPING→STOPPED | `stop` 原子 + `pthread_join` | `xr_worker_create/destroy` |
| **T task** | CREATED→READY→RUNNING→{PARKED,DEAD};PARKED→READY | parker CAS 门 + worker 循环 | `worker_run_task` / `task_deliver` |
| **P parker** | IDLE⇄PARKED⇄NOTIFIED(+V0 `pub` 发布权分轨) | 原子 CAS,无锁 | `xr_parker_park/unpark` |
| **H waker** | REGISTERED→UNREGISTERED(V0 registry;V4a 直接指针) | `worker->lock`(V0) | `task_deliver` 解析 |
| **E 事件面** | pending latch(0/1) + eventfd/futex | 原子 CAS(合并) | `worker_wake` |

> 状态机必须**显式枚举 + 原子字段 + CAS 转换**(PEL P1/§3);禁止用多个
> bool/flag 组合表达组件状态。V0 的 `state/pub/payload` 是唯一例外形态,
> 因其为无锁单对象协议(PEL §5.2 明确允许辅助原子字段)。

### 1.2 交互边与收敛点(每条边必须走对应收口)

| 交互 | 内容 | 收敛点 |
|---|---|---|
| P→T | park/suspend、wakeup reason 传递 | parker CAS 门(`xr_parker_park`) |
| H→T | deliver 经句柄解析找到 task | V0 `reg_find_locked` / V4a 直接指针,统一在 `task_deliver*` |
| T→Q | 就绪入队 | `push_ready_locked` / V3 `mpsc_push`(锁外) |
| Q→T | 调度取出 | `worker_pop` / V3 `mpsc_pop` |
| Q→E | transport(打断睡眠) | `worker_wake`(pending 合并 + 门控) |
| W→T | 停止/销毁 | `xr_worker_destroy`(stop→wake→join) |

### 1.3 收敛规则(本仓版 G1–G5)

1. **G1 触碰统一**:唤醒方不持裸 task 指针(V0);经 waker 句柄解析,**miss
   即丢**——迟到通知对已退出消费者本就该丢。V4a 直接指针变体必须写清生命期
   前置条件(handoff §3.2:task 活过所有 in-flight unpark),且仍只经
   `task_deliver_direct` 一个触碰点;
2. **G2 仲裁统一**:单次唤醒的交付权在唯一收口裁决——V0 `pub` claim(PEL
   doc97 协议),V6 单字 `swap`(last-wins);**禁止一个 parker 混用两套协议**;
3. **G3 观测统一**:stats/trace/probe 只读原子字段或调用点快照;**开门/交付
   之后禁止再读 parker 字段**(消费者可能已进入下一轮复用,PEL doc97 §6.8);
4. **G4 拆除统一**:worker 拆除固定序 = 置 `stop` → `worker_wake` → join →
   队列/registry 清空 → 释放 fd;禁止任意顺序拆;
5. **G5 生命周期单一**:每域存活证明唯一——V0 task 生命周期由调用方(栈/
   静态/池)持有,registry 只是索引不是生命周期;V4b 引用计数若做,必须
   替换而非叠加 V0 弱句柄语义。

## 2. 生命周期组合元规则(L0–L6 本仓版)

| # | 规则(PEL §0.2) | 本仓落地 | 反例(禁止) |
|---|---|---|---|
| **L0** | owner 终态 ⇒ ledger ∅ | worker STOPPED ⇒ ready 队列空、registry 空、无 in-flight deliver | destroy 返回后仍有 deliver 触碰 task |
| **L1** | 发布在观测域且最后 | parker 开门后零触碰;V0 deliver 内不读开门后字段 | deliver 后继续读 `parker->state`/`payload` |
| **L2** | 登记先于使用 | `xr_task_spawn`:先 registry 后 ready;未注册 task 不得被 deliver | 先入 ready 后注册(窗口内 miss 丢唤醒) |
| **L3** | 排空三步骨架 | worker teardown = 停接入 → drain ready → 清 registry → 发布 STOPPED | 先 join 线程后清队列(回调无出口) |
| **L4** | 唯一收口 + 抢占即回收 | registry 摘除/队列节点清理各恰一处;`XR_TASK_DONE` 摘除在 `worker_run_task` | 多路径 free/摘链 |
| **L5** | 终态静默 + 完成判据 | destroy 返回后零回调;完成用计数(stats/join)判定 | sleep/轮询兜底判完成 |
| **L6** | 拒绝所有权 | deliver 入队 miss 即丢,不接管 payload;跨线程投递返回值必须处理 | 忽略入队失败/返回码 |

## 3. 元模式 P1–P16 本仓落地表

| # | 模式 | 本仓落地 | 状态 |
|---|---|---|---|
| P1 | 显式状态机替代 bool | parker 三态/task 返回码/worker stop 原子 | ✅ 已落地 |
| P2 | 锁层次 DAG | parker 原子 → `worker->lock`(registry+ready) → transport;禁反向 | ✅(V3 去锁后更新) |
| P3 | 延迟唤醒 | `task_deliver` 锁内入队、解锁后 `worker_wake`(`xr_worker.c:143-176`) | ✅ 已落地 |
| P4 | 两类资源分策 | worker 队列/registry 加锁直访;epoll/eventfd 仅 owner 线程触碰 | ✅ 已落地 |
| P5 | 稳定槽 vs 瞬态槽 | parker `state`(协议)vs `payload/reason`(数据)分离;V4 句柄 vs registry | ✅/变体 |
| P6 | CAS 仲裁 + 辅助发布 | V0 claim 协议(PEL doc97);V6 单字 permit(契约见 §4.3) | V0 ✅ / V6 待做 |
| P7 | Close ≠ Destroy | worker `stop`(信令)≠ `free`(销毁);task DONE ≠ 立即 free | ✅ 已落地 |
| P8 | 自顶向下销毁 | worker destroy:停 task 接入 → 空队列 → 释放 fd | ✅ 已落地 |
| P9 | 自然排空 | 禁止强杀线程;stop flag + wake + join | ✅ 已落地 |
| P10 | 复用栈零初始化 | `xr_task_init` 先 `memset` 再赋值;栈上 parker 逃逸必须完整初始化 | ✅ 已落地 |
| P11 | ABI 兼容 | 本仓无发布面(内部头自由);另立运行时再启用 PEL §9.3 | N/A |
| P12 | 临界区不挂起 | 持 `worker->lock` 禁止 park/IO/transport/deliver 回调 | ✅ 已落地 |
| P13 | 单一生命周期机制 | task 生命周期由调用方唯一持有;registry/V4b 不得叠加 | ✅ |
| P14 | 观测面禁读竞争字段 | stats 原子;trace 用调用点快照;开门后 known-value | ✅(改动时复核) |
| P15 | 触碰点统一 | V0 弱句柄 miss 即丢;V4a 直接指针单点触碰 | ✅/变体 |
| P16 | 三步骨架 | 同 L3;worker teardown 固定序 | ✅ 已落地 |

## 4. 领域硬性约束(吸收 PEL §1–§10 适用部分)

### 4.1 分层与边界

分层与 L0 栈切换选型见 `02-upper-bound-design.md` §0.5;约束:机制层(L1–L3)
不得直接 include stackman,栈切换只经 `xr_ctx_switch` 抽象。

### 4.2 线程安全与锁层次(吸收 PEL §2)

- **锁层次(硬性,禁止反向)**:
  `parker 原子(pub/state CAS)` → `worker->lock`(registry + ready 队列)
  → `transport`(eventfd write / futex_wake,无锁)。
  transport 永远在锁外;`deliver` 回调持有 parker `pub` 期间禁止再取
  `worker->lock` 以外的锁、禁止 park/IO;
- **延迟唤醒(硬性)**:锁内只收集/入队,锁外唤醒。V0 `task_deliver` 即此形态;
- **临界区不挂起(P12)**:持锁期间禁止:park、阻塞 IO、`worker_wake`、
  调用 `t->fn`、重入 parker;
- **跨线程唤醒路径(本仓版)**:
  ```
  producer: unpark(claim→payload→door CAS)
    → deliver(锁内 registry 解析 + ready 入队;解锁)
    → worker_wake(pending CAS 合并;必要时 eventfd write/futex_wake)
  owner:    epoll_wait/futex_wait 返回 → read/清 pending → drain → run_task
  ```
- **sleeping 门控不丢唤醒(硬性,handoff §1.2)**:必须"先 `sleeping=1`
  (release)再复核队列";生产者顺序"入队(unlock)→ 读 sleeping";
  **GATE 依赖 mutex 队列对该协议的串行化**:V3 MPSC 无锁环破坏它
  (store-load 竞速丢唤醒,tsan 不报),故 **MPSC|GATE 同开时 GATE 自动失效**
  (统一总 transport,`xr_worker.h`/`deliver_impl`);
- **V5 futex 单字协议(硬性)**:`fut_word` 0=worker 声明睡眠、1=awake/唤醒
  在途;worker"先置 0 再复核队列,空则 `futex_wait(word==0)`";生产者
  `exchange(1)`,旧值 0 才 `futex_wake`(awake 时零 syscall);futex_wait 以
  字值原子复核,不丢唤醒。**FUTEX 模式不维护 `sleeping`**,GATE 与其同开
  自动失效;**MPSC|FUTEX 正确**(不依赖队列串行化);`set_flags` 切 FUTEX 时
  对可能睡在 epoll 的 worker 补一次 eventfd 唤醒(一次性配置期)。
- **wake_pending 清零时机(硬性,handoff §1.3)**:只在 read eventfd 之后、
  下一轮 drain 之前;否则丢唤醒。

### 4.3 Parker 协议(吸收 PEL §5.2,结合 V6 上限形态)

**V0(三态 + 发布权分轨,默认路径)**:

- `IDLE/PARKED/NOTIFIED` 是唯一状态机,不得引入破坏三态语义的额外状态;
- 生产者 claim 选举(败者零写入)→ 先写 payload → 开门 CAS(release 点);
  消费者 consume-CAS(acquire)后 payload 必然完整,零自旋;消费后按 ticket
  归属释放 claim;
- **禁止 loser 覆写 winner payload**;"谁赢得本次唤醒"由 CAS 决定;
- park 侧单等待者;deliver 在 unpark 线程、持 `pub` 时执行,**不得重入本
  parker、不得跑任务**(PEL D8 教训:唤醒发起方栈帧寿命——unpark 之后调用方
  仍有动作,直跑等于让渡整条调用栈);
- 开门后零触碰(G3);parker 复用(reset/init)仅在"上一唤醒已消费、下一源
  未启动"窗口内调用。

**V6(单字 permit,上限形态,待做)**:契约差异必须显式接受并记录——
无发布权仲裁(多生产者 last-wins 合并)、无内建 reason、允许合并/虚假唤醒,
上层须重查循环 + 外置 payload/reason。**同一 parker 只允许一种协议**。

### 4.4 task / worker / 队列(吸收 PEL §3/§6 适用部分)

- **task 不迁移**:owner 固定,执行/挂起/恢复只在其 worker 线程;跨线程仅
  deliver 入队 + transport;
- **至多一个 ready 条目不变式(handoff §1.5)**:parker 状态机保证同一 task
  任一时刻至多一个 ready 条目;V3 环形队列/新增路径必须复核该性质;
- **task 生命周期**:`XR_TASK_DONE` 由 `worker_run_task` 唯一摘除;摘除后
  禁止任何 deliver 触碰(靠 L1/L2 保证);
- **V3 MPSC 队列**:Vyukov 有界环 + 溢出链;`push` 只在锁外,`pop` 单消费者;
  溢出链仍受 `worker->lock` 保护;不得在持锁路径调用 `push`;
- **V7 LIFO slot 例外**:同 worker 直投可走 LIFO 槽(限 3 次,防饿死),但
  必须仍从 `worker_pop` 统一取任务,不得旁路。

### 4.5 基础设施(吸收 PEL §9 适用部分)

- **零初始化(P10)**:`xr_task_init`/parker 初始化必须先清零再赋值;栈上
  结构体指针逃逸(入队/回调)前完整初始化;
- **sanitizer 面(分级,2026-10-02 用户指示调整)**:默认**轻量**——`build.sh
  debug` + `run-tests.sh debug`(相关 ctest);按需单面:内存问题→asan、
  竞争/丢唤醒→tsan、UB→ubsan;**五面全量(gcc/clang Debug + asan/tsan/ubsan
  各 3/3)不再每变体强制**,改为按需/里程碑(阶段收口、提交前、或用户要求);
  stackman L0 对照轮启用时按 PEL §9.2 平台隔离(`-fno-sanitize=all` + ASan
  fiber API),不照搬产品域门禁(无 ubsan-extra)。
  > **恢复条件**:出现内存/竞争类缺陷、或进入发布/回灌评审时,恢复"核心目录
  > 改动五面全量"。
- **观测(G3/P14)**:stats 只原子更新;分段计时(TSC)在收口点打点;
- **测量纪律(吸收 01-plan §3 + PEL doc146 §0.5)**:先 `env-check.sh`
  (governor/遗留进程/绑核);同窗单变量 A/B、warmup + ≥3s、3 轮 median;
  超轮间噪声(≥5% 或分布明确前移)才保留,否则回退留档;证据链先于修法,
  perf 先于读码猜想;无靶不吃药(§0.3.2 规则数判据)。

### 4.6 变更判据(硬性前置)

每次改动回答"必须记住的规则数"是增是减:只增不减 → 先问是否存在统一收口点
替代;确认没有再动手(PEL §0.3.2)。历史基准:V1/V2 未超噪声 → 不作默认、
不迁移(handoff §2)。

## 5. 符合性保证:五面绿 ≠ 符合约束(吸收 PEL §0.6)

| 手段 | 覆盖 | 不能覆盖 |
|---|---|---|
| 五面 sanitizer | 已执行路径的内存违例/竞争 | 符合契约;未执行路径;生命周期序错 |
| 结构守卫(grep) | 收口散点/eventfd/futex/直接调 fn 等可静态判定项 | 语义正确性 |
| 对照本文件逐条复核 | 设计符合性 | 大规模运行验证 |

**硬性认知**:① 五面绿 ≠ 符合约束,验收表述用"按本文件 §x 逐条对上";② 可
守卫化的约束尽量 grep 进 CI,不靠自觉;③ 守卫随协议同生共死,协议淘汰即摘除。

## 6. 显式不适用 / 暂缓的 PEL 约束(登记,不照搬)

| PEL 约束 | 本仓处置 | 启用条件 |
|---|---|---|
| §1.2 双全局配置 | 不适用(无配置面) | 另立运行时 |
| §4 handle_reg 三槽 / handle→data / close 时序 | 不适用(无 libuv handle) | 引入真实 IO 后按需 |
| §4.4 G13 libuv 关闭纪律 / §4.5 Loop Bridge | 不适用(无 libuv/bridge) | 引入第三方 loop |
| §6.3 fibre 引用计数 / §6.4 Scope / §7 Channel-Select | 不适用(无 fibre/scope/channel) | 产品化 |
| §5.2 doc144 ctx 等待者 / `park_no_ctx` | 暂缓(无 context/cancel 面) | 加取消语义时 |
| §9.3 ABI 三阶段 / §10.1-10.2 状态宏/lifecycle / §10.5 magic | 暂缓(无发布面;内部头自由) | 另立运行时 |
| §9.4 TRACE P0 覆盖 | 暂缓(有 stats/TSC 观测) | 产品化 |
| stranger_sweep / teardown walk / L4 唯一 free 守卫 | 不适用(无第三方 handle) | 产品化 |

> 处置原则:**不适用=本仓没有对应组件,不是豁免**;一旦组件引入,必须按
> PEL 00 对应节重新落地并适配,不得就地发明新规则(PEL §0.3.2)。

## 7. 参考

- PEL `00-unified-lifecycle-design.md` §0(六机/G1–G5/L0–L6/P1–P16/§0.3.2/
  §0.6)与 §2/§5.2/§9(吸收来源);
- PEL doc97(发布权 claim)/ doc99(弱句柄)/ doc120(句柄原子化)/
  doc125(repeatable 统一)/ doc144(ctx 等待者)/ doc146 §0.5(方法学)/
  doc151 §4.3(对照数字)/ doc160-161(vstack/模型税);
- 本仓 `01-plan.md`(计划/判据/台账)· `02-upper-bound-design.md`(上限路径/
  统一收口设计)· `03-handoff-s4-s6.md`(实施交接/坑清单)。
