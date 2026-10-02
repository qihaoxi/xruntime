# xruntime 00-unified-constraints — 约束总纲(吸收 PEL 00,结合本仓实际)

日期:2026-10-02(2026-10-03 重排两层) · 状态:生效 · 性质:**本仓约束唯一正文**

> 来源:PEL `polyglot-c/docs/00-unified-lifecycle-design.md` v9.0(六机与交互边
> G1–G5、组合元规则 L0–L6、元模式 P1–P16、领域硬性约束、符合性保证)。
> 本仓是**机制沙盒 + 上限路径**,不是产品运行时:只吸收与"唤醒链机制实验"
> 相关的约束,并按本仓实际(task/worker/parker/registry/queue/transport、
> V0–V9、B1–B5)重新落地;**产品域组件(libuv handle/channel/scope/fibre
> 引用计数/ABI 发布面)显式不适用**(§6 登记)。
>
> **阅读方式(两层)**:
> - **速查层 = 第一部分 §0–§6**:元规则、元模式、硬性约束,**默认只读此层**,
>   按表执行;实现/审查不需要读第二部分。
> - **详述层 = 第二部分 附录 A–H**:为什么、反例、实例、协议论证;
>   深入某机制设计/排查时按需查阅,**勿整文入上下文**。
>
> 关系:`01-plan.md`=计划/判据/台账 · `02-upper-bound-design.md`=上限路径设计 ·
> `03-handoff-s4-s6.md`=实施交接 · `05-measurement-toolbox.md`=测量工具 ·
> **本文件=约束唯一正文**;冲突时以本文件条款 + 同窗实测为准。

---

# 第一部分 · 元规则与硬性约束(速查层,默认只读)

## 0. 首要原则:统一收口(单一漏斗,硬性)

> **同一功能只允许一个权威收口点(漏斗);所有调用路径必须经过它,禁止在
> 调用点分散实现/判断。** 加 AOP/横向切面(观测、trace、统计、sanitizer hook、
> 计费)只在一点挂接;变体替换只改漏斗内部,调用点与上层语义零改动。

**四条推论(硬性)**:① 单点横切——probe/trace/stats 只挂收口点;② 单点证明——
状态机/内存序/生命周期证明集中漏斗内;③ 单点变体——V0→V9 只换漏斗内部;
④ 规则数判据——收口使规则数净减(§0.3.2 同型;详见附录 A)。

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

## 1. 状态机与收敛点(G1–G5)

### 1.1 本仓状态机清单

| 机 | 状态 | 转换保护 | 权威入口 |
|---|---|---|---|
| **W worker** | CREATED→RUNNING→STOPPING→STOPPED | `stop` 原子 + `pthread_join` | `xr_worker_create/destroy` |
| **T task** | CREATED→READY→RUNNING→{PARKED,DEAD};PARKED→READY | parker CAS 门 + worker 循环 | `worker_run_task` / `task_deliver` |
| **P parker** | IDLE⇄PARKED⇄NOTIFIED(+V0 `pub` 发布权分轨) | 原子 CAS,无锁 | `xr_parker_park/unpark` |
| **H waker** | REGISTERED→UNREGISTERED(V0 registry;V4a 直接指针) | `worker->lock`(V0) | `task_deliver` 解析 |
| **E 事件面** | pending latch(0/1) + eventfd/futex | 原子 CAS(合并) | `worker_wake` |

> 硬性:状态机必须**显式枚举 + 原子字段 + CAS 转换**;禁止用多个 bool/flag
> 组合表达组件状态。V0 的 `state/pub/payload` 是唯一例外形态(无锁单对象
> 协议,PEL §5.2 允许辅助原子字段);理由见附录 B。

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
   即丢**。V4a 直接指针变体必须写清生命期前置条件(task 活过所有 in-flight
   unpark),且仍只经 `task_deliver_direct` 一个触碰点;
2. **G2 仲裁统一**:单次唤醒的交付权在唯一收口裁决——V0 `pub` claim(PEL
   doc97 协议),V6 单字 `swap`(last-wins);**禁止一个 parker 混用两套协议**;
3. **G3 观测统一**:stats/trace/probe 只读原子字段或调用点快照;**开门/交付
   之后禁止再读 parker 字段**(消费者可能已进入下一轮复用);
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

> 反例走查与"为什么"见附录 C。

## 3. 元模式 P1–P16 本仓落地表

| # | 模式 | 本仓落地 | 状态 |
|---|---|---|---|
| P1 | 显式状态机替代 bool | parker 三态/task 返回码/worker stop 原子 | ✅ 已落地 |
| P2 | 锁层次 DAG | parker 原子 → `worker->lock`(registry+ready) → transport;禁反向 | ✅(V3 去锁后更新) |
| P3 | 延迟唤醒 | `task_deliver` 锁内入队、解锁后 `worker_wake` | ✅ 已落地 |
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

## 4. 领域硬性规则速查

### 4.1 分层与边界

- 分层与 L0 栈切换选型见 `02-upper-bound-design.md` §0.5;
- **硬性**:机制层(L1–L3)不得直接 include stackman,栈切换只经
  `xr_ctx_switch` 抽象。

### 4.2 线程安全与睡眠门控(丢唤醒红线)

- **锁层次(硬性,禁止反向)**:parker 原子(pub/state CAS)→ `worker->lock`
  (registry + ready 队列)→ transport(eventfd write / futex_wake,无锁);
  transport 永远在锁外;deliver 持 parker `pub` 期间禁止再取 `worker->lock`
  以外的锁、禁止 park/IO;
- **延迟唤醒(硬性)**:锁内只收集/入队,锁外唤醒(P3);
- **临界区不挂起(P12)**:持锁期间禁止 park、阻塞 IO、`worker_wake`、调用
  `t->fn`、重入 parker;
- **跨线程路径(固定序)**:unpark(claim→payload→door CAS)→ deliver(锁内
  解析+入队;解锁)→ `worker_wake`(合并/门控);owner 侧 wait 返回 → 清
  pending → drain → run_task(时序与竞态见附录 D);
- **sleeping 门控不丢唤醒(硬性)**:必须"先 `sleeping=1`(release)再复核
  队列";生产者顺序"入队(unlock)→ 读 sleeping";`wake_pending` 只在 read
  eventfd 之后、下一轮 drain 之前清零;
- **GATE 依赖 mutex 队列串行化**:MPSC 无锁环破坏它(store-load 竞速丢唤醒,
  tsan 不报),**MPSC|GATE 同开时 GATE 自动失效**;
- **V5 futex 单字协议(硬性)**:`fut_word` 0=worker 声明睡眠、1=awake/唤醒
  在途;worker"先置 0 再复核队列,空则 `futex_wait(word==0)`";生产者
  `exchange(1)`,旧值 0 才 `futex_wake`(awake 时零 syscall)。**FUTEX 不维护
  `sleeping`**,GATE 与其同开自动失效;**MPSC|FUTEX 正确**;`set_flags` 切
  FUTEX 时对可能睡在 epoll 的 worker 补一次 eventfd 唤醒(一次性配置期)。

### 4.3 Parker 协议

- **V0(默认)**:`IDLE/PARKED/NOTIFIED` 唯一状态机;claim 选举(败者零写入)
  → 先写 payload → 开门 CAS(release 点);消费 consume-CAS(acquire)后按
  ticket 归属释放 claim;禁止 loser 覆写 winner;**deliver 不得重入本 parker、
  不得跑任务**(唤醒发起方栈帧寿命红线);开门后零触碰(G3);parker 复用仅
  在"上一唤醒已消费、下一源未启动"窗口;
- **V6(单字 permit,上限形态,待做)**:无发布权仲裁(last-wins 合并)、无内建
  reason、允许合并/虚假唤醒,上层须重查循环 + 外置 payload/reason;
  **同一 parker 只允许一种协议**;
- 协议论证与 D8 栈帧寿命教训见附录 E。

### 4.4 task / worker / 队列

- **task 不迁移**:owner 固定,执行/挂起/恢复只在其 worker 线程;跨线程仅
  deliver 入队 + transport;
- **至多一个 ready 条目不变式**:parker 状态机保证;V3 环形队列/新增路径必须
  复核该性质;
- **task 生命周期**:`XR_TASK_DONE` 由 `worker_run_task` 唯一摘除;摘除后禁止
  任何 deliver 触碰(靠 L1/L2 保证);
- **V3 MPSC**:Vyukov 有界环 + 溢出链;`push` 只在锁外,`pop` 单消费者;溢出链
  受 `worker->lock` 保护;不得在持锁路径调用 `push`;
- **V7 LIFO slot 例外**:同 worker 直投可走 LIFO 槽(限 3 次防饿死),但必须
  仍从 `worker_pop` 统一取任务,不得旁路。

### 4.5 基础设施与门禁

- **零初始化(P10)**:`xr_task_init`/parker 初始化先清零再赋值;栈上结构体
  指针逃逸(入队/回调)前完整初始化;
- **sanitizer 分级(2026-10-02)**:默认 `build.sh debug` + `run-tests.sh
  debug`;按需单面:内存→asan、竞争/丢唤醒→tsan、UB→ubsan;**五面全量按需/
  里程碑**(阶段收口、提交前、用户要求),不再每变体强制;stackman L0 对照轮
  按 PEL §9.2 平台隔离;
  > **恢复条件**:出现内存/竞争类缺陷、或进入发布/回灌评审时,恢复全量。
- **观测(G3/P14)**:stats 只原子更新;分段计时(TSC)在收口点打点;
- **测量纪律**:先 `env-check.sh`;同窗单变量 A/B、warmup + ≥3s、3 轮 median;
  超轮间噪声(≥5% 或分布明确前移)才保留,否则回退留档;证据链先于修法,
  perf 先于读码猜想;无靶不吃药。工具见 `05-measurement-toolbox.md`。

### 4.6 变更判据(硬性前置)

每次改动回答"必须记住的规则数"是增是减:只增不减 → 先问是否存在统一收口点
替代;确认没有再动手。历史基准:V1/V2 未超噪声 → 不作默认、不迁移。

## 5. 符合性保证:五面绿 ≠ 符合约束

| 手段 | 覆盖 | 不能覆盖 |
|---|---|---|
| sanitizer 面 | 已执行路径的内存违例/竞争 | 符合契约;未执行路径;生命周期序错 |
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
| §9.3 ABI / §10.1-10.2 状态宏/lifecycle / §10.5 magic | 暂缓(无发布面;内部头自由) | 另立运行时 |
| §9.4 TRACE P0 覆盖 | 暂缓(有 stats/TSC 观测) | 产品化 |
| stranger_sweep / teardown walk / 唯一 free 守卫 | 不适用(无第三方 handle) | 产品化 |

> 处置原则:**不适用=本仓没有对应组件,不是豁免**;一旦组件引入,必须按
> PEL 00 对应节重新落地并适配,不得就地发明新规则。

---

# 第二部分 · 按需详述(为什么/反例/实例,默认不读)

## 附录 A. 统一收口:理由与反例

- **为什么**:散点实现的代价有三类——① 加观测要改 N 处且口径漂移(同一
  "唤醒次数"在两个调用点各写一遍,迟早对不上);② 证明分散(状态机不变量
  在 A 处成立、B 处被绕过,review 要追 N 条路径);③ 变体无法单变量(A/B
  要求只改一处,散点必须同步改 N 处,遗漏即伪结论)。
- **反例**:transport 若允许各适配器自己 `write(event_fd)`,V5 换 futex 时
  必然漏掉某条路径(该路径静默退回 eventfd,基准数字互相矛盾);
  park 判定若允许各 API 自己 CAS,三态协议就出现第二份实现,内存序证明失效。
- **落地判据**:任何新调用点先问"这是否是某个已有漏斗的职责";是→走漏斗;
  不是→先定义漏斗,再接入(constraints §0 守卫)。

## 附录 B. 状态机与收敛点:本仓实例

- **为什么显式枚举**:N 个 bool 有 2^N 组合,多数非法且编译器不拦;单枚
  原子 state 的状态空间=合法状态数,可 dump/可断言(PEL P1)。
- **V0 `state/pub/payload` 例外**:parker 是无锁单对象协议,`pub` 是发布权
  仲裁位不是状态位,`payload` 是数据槽;语义由 PEL doc97 的 claim 协议证明,
  不引入"额外状态"(§1.1 注)。
- **G1 miss 即丢**:迟到通知对已退出消费者按定义无意义;miss-drop 把"对象
  已死"从 UAF 变成正常分支(V0 registry 由 `worker->lock` 保护,
  `reg_find_locked` 找不到即返回)。
- **G2 claim vs swap**:V0 需要"多生产者 first-wins + payload 完整" →
  claim(ticket)+开门 CAS;V6 接受 last-wins → 单字 swap 即可;协议一旦选定
  不可混用(混用会让 loser 覆写 winner payload)。
- **G3 实测来源**:PEL doc97 §6.8——开门(发布完成)之后再读 parker 字段,
  消费者可能已消费并 `init` memset 进入下一轮,tsan 实锤;
  xruntime 的对应约束是"开门后零触碰"。
- **G4/G5 走查**:destroy 先置 stop 再 wake 再 join(否则 worker 睡死);
  registry 只是弱句柄索引,不承担生命周期(否则 V4a 直接指针变体无法成立)。

## 附录 C. 生命周期元规则:反例走查

- **L0**:worker STOPPED 后仍有 deliver 触碰 task = 桩上 UAF;判据是
  destroy 返回前 join + 队列/registry 清零。
- **L1**:开门后读 `parker->state` 的典型窗口——deliver 回调返回后调用方继续
  写日志/统计(D8 同族)。
- **L2**:先入 ready 后注册的窗口内发生 unpark → `reg_find_locked` miss →
  deliver 丢弃 → 任务永远 sleep(tsan 不报的丢唤醒)。
- **L3/L4**:worker teardown 先 join 会导致队列无出口;registry 摘除若同时
  存在于 DONE 与 destroy 两条路径,必须有唯一收口(现为 `worker_run_task`)。
- **L5**:完成判据用 join/stats 计数,禁止 sleep 猜;否则慢机假绿、快机假红。
- **L6**:deliver 的 miss 分支必须"不接管 payload、丢弃";跨线程投递返回值
  被忽略 = 规则数膨胀(PEL L6 同型)。

## 附录 D. 睡眠门控与竞态窗口(为什么这样写)

- **延迟唤醒的目的**:锁内唤醒会在锁层次上反向(worker->lock → parker 的
  调用方)并制造嵌套锁;锁内只入队,锁外 transport。
- **sleeping 门控丢唤醒窗口**(无 GATE 复核时):
  ```
  worker: pop 空 → [生产者入队 + 读 sleeping==0 → 跳过唤醒] → epoll_wait 睡死
  修复:worker 先 sleeping=1(release) 再复核队列;生产者入队后读 sleeping,
        若见 0 说明入队发生在复核之前 → worker 复核必见任务(靠 mutex 串行化)
  ```
- **GATE×MPSC 竞速**:MPSC 无锁环使"入队 vs 复核"不再同锁串行——
  worker `sleeping=1` 与 producer `load sleeping` 构成 store-load 竞态,
  双方可各自读到旧值而丢唤醒(实测:debug 偶发、tsan 不报);故
  MPSC|GATE 时 GATE 自动失效。
- **futex 单字协议正确性**:worker 先 `store(0)` 声明睡眠再复核队列,
  producer `exchange(1)` 旧值 0 才 `futex_wake`;三种交错——① exchange 在
  store(0) 前:worker 复核见任务,不睡;② 在 store(0) 后、`futex_wait` 前:
  syscall 空唤醒,`futex_wait` 见字值 1 返回 EAGAIN;③ 在 wait 中:正常唤醒。
  `futex_wait` 以字值做原子复核是"不丢唤醒"的最后一环,不可省。
- **`set_flags` 双写**:worker 可能已按旧 flags 睡在 epoll,而新 transport 走
  futex——配置期同时 eventfd 写 + futex_wake 覆盖两种睡眠原语,worker 醒来
  重读 flags;热路径无此开销。

## 附录 E. Parker 协议详解

- **V0 claim(doc97)**:`pub_ticket=fetch_add` 取号 → `pub_claim` CAS 抢占;
  败者零写入立即返回;胜者写 `data/reason` 后做开门 CAS(→NOTIFIED)——
  开门是 release 点,消费 CAS 是 acquire 点,payload 可见性零自旋;消费后按
  自己观测到的 ticket 归属释放 claim(防误清下一轮)。
- **D8 栈帧寿命红线(PEL doc152)**:所有 unpark/notify 调用点在调用之后
  几乎都有后续动作(完成钩子、timer 循环、统计);把"异步生效"改成"同步
  直跑"会让被唤醒者 free 掉发起方还在使用的帧。本仓对应规则:parker 的
  `deliver` 回调内不得跑任务/不得重入 parker;同线程直投(V1)只入队,
  在 step 边界由 worker 取。
- **V6 取舍**:单字 permit 省掉 claim/payload/自旋(每对 2 RMW),代价是
  last-wins 合并、无 reason、允许虚假唤醒——取消/超时/精确 reason 需要外置
  字段 + 上层重查循环;这是上限路径的显式能力交换,不默认启用。

## 附录 F. 队列与 task 详解

- **Vyukov MPSC**:`tail.fetch_add` 取位 + slot `seq` release 发布;pop 检查
  `seq==pos+1` 后置 `seq=pos+capacity`;预检 `tail-head>=capacity` 满则走溢出
  链(不中途放弃已 claim 位,否则留洞使消费者卡住)。
- **溢出链**:`worker->lock` 下的 ready 链表;V3 下它是唯一持锁路径,pop 先环
  后链;RUN_AGAIN 重排与 deliver 同走 `worker_enqueue` 漏斗。
- **LIFO slot(V7)**:同 worker 直投优先跑下一任务(限 3 次),提高局部性并防
  饿死;仍经 `worker_pop` 统一取,不得旁路。
- **task 不迁移的原因**:stackless 续体 + 栈上 parker/ctx 在所有者线程外触碰
  需要全局生命周期证明;沙盒选择 owner 固定 + 弱句柄(miss 即丢)收敛之。

## 附录 G. 门禁与测量详解

- **五面各自盲区**:asan 抓已执行路径的内存违例(自定义栈帧 UAF 零覆盖);
  tsan 抓已执行路径的竞争(丢唤醒窗口、store-load 竞速未必报);ubsan 抓 UB;
  结构守卫只抓可静态判定项;**符合性必须对照本文件逐条复核**(§5)。
- **分级门禁理由(2026-10-02)**:五面全量对每个小改动过重,且本仓为沙盒;
  默认 debug+相关 ctest 保障功能,按需单面定靶,阶段收口再全量。
- **测量方法学**:环境先于归因、同窗单变量、交错消除漂移、3 轮 median、
  ≥5% 判据、负向对照、证据链先于修法、perf 先于读码猜想;完整工具与命令见
  `05-measurement-toolbox.md`。

## 附录 H. 参考

- PEL `00-unified-lifecycle-design.md` §0(六机/G1–G5/L0–L6/P1–P16/§0.3.2/
  §0.6)与 §2/§5.2/§9(吸收来源);
- PEL doc97(发布权 claim)/ doc99(弱句柄)/ doc120(句柄原子化)/
  doc125(repeatable 统一)/ doc144(ctx 等待者)/ doc146 §0.5(方法学)/
  doc151 §4.3(对照数字)/ doc152(D8)/ doc160-161(vstack/模型税);
- 本仓 `01-plan.md` · `02-upper-bound-design.md` · `03-handoff-s4-s6.md` ·
  `04-final-report.md` · `05-measurement-toolbox.md`。
