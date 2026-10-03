# xruntime 06 — PEL 修改评估文档：transport 回灌 + park 次数削减

日期：2026-10-03 · 状态：**提案（已被 PEL 实测裁定修正，见 §0）** ·
证据：xruntime S1–S9 沙盒同窗实测 + PEL doc165/166 eBPF/perf 实测
（`bench-logs/`、`test-logs/`）

> **性质声明**：本文是**评估材料**，不是修改授权。所有沙盒数字为
> stackless、无 libuv、governor=powersave 的**相对值**；回灌 PEL 必须走
> PEL 核心目录设计+评审门禁（doc155 §7.7 反绕过纪律），并以 PEL 侧同窗
> 单变量 A/B 为最终判据。沙盒结论不构成对 PEL 任何目录的修改依据。

---

## 0. PEL 侧实测裁定（doc165/166，2026-10-03）——对本文结论的修正

> **本节优先于后文所有外推**。PEL 已用 eBPF（doc165）与 perf 归因（doc166）
> 闭环了本文最关键的假设，结论与 xruntime 沙盒外推有实质差异。

### 0.1 唤醒是 self-wake，不是跨线程（doc165 §2.2）

- PEL 服务形态（echo c=1/16/256、http c=1、chan 微基准、多 worker）：
  `uv_async_send` 调用 tid 与 `epoll_pwait` tid **完全一致**，跨线程唤醒占比
  **~0%**（http c=1 仅 0.025%，来自管理线程）。
- 含义：eventfd 在 PEL 请求路径上的作用不是"跨线程投递"，而是**打断本线程
  `epoll_wait`，让 ready_queue 在本轮被 drain**（D8：`uv_check` 已过本 tick，
  不能同线程直投）。**沙盒 futex 的跨线程 −88% 收益不直接适用**。
- **M1 裁定：现有请求形态下 ROI ≈ 0**，不作为第一优先；仅保留给"真跨线程
  通知"（thread bridge/microtask/外部线程 API），且需先有量级证据；形态 B
  （idle-loop futex）才有机会命中 c=1 的 1.21 eventfd/req，但必须做 hybrid
  窗口竞态专项测试（doc165 §3.3/§7）。

### 0.2 每请求挂起数与即时消费（doc165 §2.2）

| 口径 | echo c=1 | echo c=16 | echo c=256 | http c=1 |
|---|---|---|---|---|
| 实际挂起/req | 2.11 | 1.21 | 1.18 | 2.29 |
| 实际 eventfd 写/req（合并后） | 1.21 | 0.076 | 0.0049 | 1.20 |
| read/write EAGAIN | 0 | 0 | 0 | 0 |
| 阻塞调用/req | 2.40 | 2.40 | — | — |
| 即时消费（未挂起） | **0.38%** | ~50%（send 半边） | — | — |

- **try-before-park 在同调用数下几无靶**（c=1 即时消费仅 0.38%）；M2 的杠杆
  是**减少阻塞调用数**（send 快路径/批量提交），且受 D9 用户裁决约束：
  libuv 写完成回调是完成确认点，禁止 tokio 式纯异步写（doc166 边界）。
- 高并发下 eventfd 已被 libuv pending 合并到 0.076→0.0049/req，**c≥16 的
  差距与唤醒无关**（doc165 §2.2，与 doc148 D9 同判）。

### 0.3 收益上界（doc165 §2.2，同窗 PEL vs libuv-raw）

| echo | PEL | libuv-raw | PEL/uv | 绝对差 |
|---|---|---|---|---|
| c=1 | 68960 rps / p50 13.2µs | 83646 / 11.1µs | 0.82 | **+2.1µs** |
| c=16 | 160839 / 92.8µs | 212145 / 70.9µs | 0.76 | +32% |
| c=256 | 141813 / 1636µs | 221060 / 1107µs | 0.64 | +56% |

- **c=1：PEL 额外开销 ≈ 2.1µs/req（阻塞模型 + 2.4 挂起 + 1.2 self-wake）
  = M2/self-wake 类优化的总上界 ≈ +20%**；不是本文早先外推的 c=1 +10~35%
  再叠 U9 +4~8×。
- U9 曲线（+4~8×）**不可外推 PEL**：沙盒 echo 的 park 发生在两个不同 worker
  之间（跨线程 handoff），PEL 是同一 loop 线程的 self-wake + 阻塞调用数；
  U9 只证明"跨线程 park 次数与吞吐近似反比"这一机制事实。

### 0.4 高并发差距的归因（doc166，c=256）

- 单 worker 饱和是 subject 构造（1 线程 99.9%），PEL vs uv 是**同口径单
  loop** 对比，0.64–0.76× 不是单核 vs 多核假差；多线程对标需
  per-connection migrate/per-channel listener（独立课题）。
- perf：差距主体 = **PEL 用户态 16.95% vs uv 2.56%**，且集中在可定位符号：
  - **观测面 ~3.8%**（uprobe 监听检查、日志判级、vdso 取时）——低风险；
  - **阻塞机件 ~6.4%**（continuation_yield、run_once、parker、io complete
    等）——中风险，需行级下钻单变量 A/B；
  - 其余 ~6.7% 分散。
- **已落地（doc166 §3.5）**：观测面批（日志判级内联镜像 + uprobe 监听快
  路径）→ libpel 用户态 16.95%→14.73%（−2.2pt），rps +1.0%，p50 −2%；
  timer `dl_now_us` 改动因无符号证据**已回退**。
- **预期**：观测面+机件合计回收 ~5–7% → c=256 **0.75→0.80~0.83×**；
  到 1.0× 仍需事件面/分发形态（更大课题）。

### 0.5 修正后的优先级（取代 §3.4 的实施顺序建议）

1. **P1 观测面回收**（低风险；doc166 已做一半）；
2. **P2 阻塞机件行级定位**（doc166 §3.1b，中风险，走三面门禁）；
3. **P3 多 worker 分发**（若目标含多线程对标：migrate/per-channel listener）；
   **U11 已验证边界**（04 §3.9）：跨线程 resume 周期 57~58ns（与同线程
   12~16ns 同量级，栈在进程 VA 天然可迁移），但线程 id/TLS/缓存句柄随线程
   变化 → 真实成本在**在途注册重绑 + TLS 契约**，不在栈；故优先 accept 时
   分发（不搬 handle），迁移只作再平衡；
4. **P4 M2 减少阻塞调用数**（send 快路径/批量；受 D9 约束，需产品确认完成
   粒度语义）；
5. **P5 M1 futex**：仅"真跨线程通知"或形态 B/C 另案评审，现有形态不作为
   第一优先；
6. **P6 M3 io_uring**：形态 C/事件面替代面，先量 D7 后剩余。

### 0.6 分阶段去线程绑定（唯一收敛点）

> 目标：**暂不做迁移**，但把线程绑定逐步收敛为"一个可 CAS 的 owner + 一个
> rebind 漏斗"，使迁移（或 accept 分发）未来只需动收敛点，不必大爆炸重写。
> 与 00 约束的"唯一漏斗"同构：每个功能唯一入口，线程亲和唯一出口。

#### 0.6.1 线程绑定普查（S0，先做）

没有这张表，"逐步"一定漏。请求路径上的状态按四类登记：

| 类别 | 例子（PEL） | 迁移处置 |
|---|---|---|
| **不可变** | 常量、只读配置、代码 | 任意线程安全 |
| **per-worker（可收敛）** | scheduler 状态、ready 队列、registry、loop 指针、pending 合并字 | 收进 `pel_exec_t`；迁移=换指针 |
| **per-fibre（应堆化）** | parker、注册句柄、timer/io 在途表、owner | 堆对象 + 原子 owner；rebind 漏斗 |
| **per-thread 叶子（无法堆化）** | `errno`、per-thread allocator arena、OpenSSL error queue、`pthread_self` 缓存、libuv 内部 TLS | 契约/审计或 save-restore；**不得跨 park 缓存** |

- 工具：doc166 perf + U11 探针法（noinline + asm memory clobber，防编译器把
  `pthread_self`（glibc `const`）与 TLS 地址跨 suspend CSE——U11 已实测）。
- 产出：状态清单 + 每项归属 + 迁移语义标注；作为后续门禁的输入。

#### 0.6.2 S1 收敛（低风险，可先做）

1. 所有 per-worker 运行时状态收进单一 `pel_exec_t`（堆），调用方只持指针，
   **不新增 `__thread`**（lint 门禁，仿 G 规则；允许清单仅不可变/叶子）；
2. `fibre_context` 堆化 + **原子 owner 字段**；parker/registry/在途表都挂在
   它下面，owner 是唯一权威（TLS 里的 worker 指针降级为"非权威提示"，
   如 xruntime 的 `tls_worker`）；
3. **rebind 唯一漏斗**：迁移/换 owner 时的注册重绑只允许经此函数，禁止
   分散在各模块；
4. 判据：无功能变化；同窗 A/B 确认间接层开销（预期每热路径几 ns，需实测）。

#### 0.6.3 S2 静默迁移契约（只有需要再平衡时才做）

- **可迁点定义**：`已挂起 && 无在途 handle/timer/IO` 的 fibre 才可迁；
  有在途 IO 的 fibre **钉住**，在其完成回调里做 CAS+rebind（完成回调本身
  在旧 loop，重绑后把 fibre 投到新 owner 队列）。
- 不变式：owner CAS 与在途表 rebind 在同一漏斗内完成；迁移窗口内唤醒
  必须经 owner 字段路由（丢唤醒注入测试 + tsan）。
- 这与 U11 结论一致：切换层便宜（跨线程 resume 周期 57ns，栈在进程 VA），
  成本全在注册重绑与 TLS 契约（§0.6.1 的第四类）。

#### 0.6.4 S3 accept 分发（最便宜，且不依赖 S1/S2）

- per-channel listener / SO_REUSEPORT / round-robin 到多 loop：连接从生到
  死都在一个 worker，**不搬 handle、不迁移**；这是 Netty 式答案，也是
  doc166 §1 指向的方向。
- **U12 已验证（04 §3.10）**：256 连接下 K=1→4 近线性（3.8×、零迁移），
  K=8 换更大客户端达 866K rps；reuseport（内核 hash 不匀）与单 acceptor
  round-robin（完全均匀、每连接一次性 handoff）两条路等效。
- 与 S1/S2 独立：先做 S3 拿容量，S1 并行收敛结构，S2 只在"必须再平衡"
  的证据出现后立项。

#### 0.6.5 S4（可选）work stealing

- 只有再平衡有数据才做；原语已在 xruntime 验证：owner CAS + MPSC 入队
  （V3）+ futex 唤醒（V5）。不做也不影响 S1–S3。

#### 0.6.6 成本、风险与门禁

| 项 | 内容 |
|---|---|
| 成本 | owner 间接（几 ns/热路径，A/B）；S1 重构面广但语义不变 |
| 风险 | **假安全**：堆化做完但 TLS 契约没做 → 迁移偶发错数据；故 S1 与 S0/S2 必须捆绑 |
| 门禁 | 禁新增 `__thread`（lint）；rebind 唯一漏斗（code review + 测试）；每阶段同窗 A/B + 三面 sanitizer |
| 回滚 | 每阶段保留旧路径/开关；S3 可独立回退 |

#### 0.6.7 与 M1/M2 的关系

- 完全正交：M1/M2 是唤醒与调用数（现有单 worker 形态）；S1–S4 是多核
  分发能力。P1/P2（doc166 用户态回收）不需要它们；只有"多线程对标"目标
  才触发 S3→S1→S2 的顺序。

### 0.7 锁-free 同线程模型的理论上界（U13 探索中）

> PEL 的设计目的：fibre 同线程 ⇒ 上游业务**无锁并发**（单线程语义保证）。
> 本节把该模型的上界写清楚，并用沙盒逐步验证。

**模型公理**

1. 每个连接/任务生命周期内固定在一个 loop 线程（thread-affine）；
2. 业务代码可无锁（无原子/无锁/无共享可变状态）；
3. 多核扩展靠 accept 时分发（连接随 loop 终身），不迁移；
4. 跨 loop 交互只能消息传递（显式、无共享）。

**上界公式**

```
总吞吐上界 ≈ min( N核 × 1/per-core成本,  共享资源上限 )
per-core成本 = loop调度 + IO/syscall + park/wake + 业务
```

- 无锁保证：省原子/锁/缓存行弹跳 → per-core 成本更低；
- thread-affine：无迁移、无跨核同步 → 省协调成本；
- 代价：self-wake（D8，~1.2/req）、挂起次数（c=1 ~2.4/req）、每 op 机件
  （doc166 6.4%）；
- 共享资源上限：内存带宽、内核 softirq/NIC、fd、客户端。

**已获得证据**

- U12（04 §3.10）：accept 分发 K=1→4 近线性（3.8×），K=8 866K rps，
  **零迁移**；
- U13a（04 §3.11）：同一服务 K=8 下 local **4.8×** vs atomic **1.1×**
  （平掉）、mutex 单锁上限 ~500K → 无锁语义的 per-core 收益随核数放大；
- U13b（04 §3.12）：跨 loop 消息（唯一跨核原语）单向 0.36µs（futex）/
  2.92µs（eventfd），吞吐 2–6M msg/s；
- U13c（04 §3.12）：本机 loopback 16 核峰值在 K=8（1.11M rps），K≥12 回落
  （客户端+softirq 争核，sys 占 server CPU ~90%）→ **共享资源先于核数
  截断**；
- U13d（04 §3.12）：D8 self-wake 代理 757ns × 1.21/req ≈ 0.92µs/req
  ≈ c=1 总成本的 7%、PEL-vs-uv 缺口的 ~44%（上界）；
- doc165/166：PEL 单 loop 0.64–0.82× uv；用户态 16.95% vs 2.56%。

**推论（U13 收束）**

- `上界 = min(N核 × per-core, 共享资源)`：per-core ≈ 0.8–0.9× uv，
  无锁红利随核数放大，分发近线性直到内核/客户端截断；
- 本机 loopback 的截断点在 ~8 loop / 1.1M rps（syscall/softirq 主导）；
  真实部署的上界取决于 NIC 多队列/独立客户端/少 syscall（io_uring）；
- 对 rust 的 0.37–0.69× 主要是单 worker vs 多线程；分发补齐后，上界由
  per-core 效率与共享资源决定；
- 该模型的目标不是"每核 1.0× uv"，而是"每核接近 uv + 无锁业务红利 +
  近线性扩展"。

**U13 计划（已完成）**

- **U13a** 无锁语义 per-core 收益 ✅（04 §3.11）；
- **U13b** 跨 loop 消息延迟/吞吐（futex/eventfd）✅（04 §3.12）；
- **U13c** 16 核 scaling 饱和点 ✅（04 §3.12：K=8 峰值，内核/客户端截断）；
- **U13d** D8 self-wake 上界 ✅（04 §3.12：757ns×1.21 ≈ 0.92µs/req）。

### 0.8 栈分配路线（U18，2026-10-04）

> 针对 stackful 的三项规模税：8MB VA/fibre、2 VMA/fibre、创建 51×。

| 指标（16KB 栈） | mmap+guard | arena（无 guard） | arena+UFFD-WP |
|---|---|---|---|
| 创建 ns/个 | 7544–8263 | **340–428** | 9825–10517 |
| VMA @30K / @100K | 60023 / 撞墙 | 26 / **26** | 28 / 28 |
| ring rtt p50 | 3516（30K） | 3626（100K） | 3687（100K） |
| RSS reside @100K | — | 422MB | 822MB |
| 溢出保护 | ✅ | ❌ | ✅ |

- **关键机制**：相邻匿名映射被内核合并 → 无 guard 时 VMA≈O(1)（guard 的
  PROT_NONE 分界是 VMA 唯一来源）；**UFFD-WP 对未 present 页不产生事件**
  （实测）→ guard 必须 touch+ioctl，创建 ~10µs、每 fibre 1 页 RSS；
- **决策**：
  1. **release=arena 无 guard**：创建 340ns、VMA≈O(1)、rtt +3%；接受
     溢出风险（或仅对显式申请大栈的 fibre 加 guard）；
  2. **debug/CI=arena+UFFD-WP**（或 mmap+guard）：捕获溢出；
  3. **100K+ 且必须安全**：UFFD-WP（10µs/个）或 **stackless**（无栈，
     顺带解决动态增长/可迁移）；
- **对 PEL**：8MB 默认栈应改为"上限/显式申请"（默认 64–128KB）；池化+
  arena 消掉创建/VMA 墙；安全与动态增长二选一，最终指向 stackless。

**直答三问（评审用）**

1. **省多少 VMA？** 从 2 个/fibre → **~0 个/fibre**：30K 时 60023→26/28；
   100K 时 mmap+guard **建不出来**（65530 墙，~3.3 万即失败），
   ARENA/ARENA_GUARD 只要 26/28 个。ARENA_GUARD 与纯 ARENA 一样省
   （UFFD-WP 是 PTE 级写保护，不拆 VMA）。
2. **越界保护有吗？** ARENA **没有**（静默踩相邻栈）；ARENA_GUARD **有**
   （实测 faults=1），代价 = 创建 ~10µs（WP 对未落页无效，必须
   touch+ioctl）+ 每 fibre 1 页 RSS；mmap+guard 有（SIGSEGV）但 2 VMA/fibre
   撞墙。
3. **需要特权吗？** ARENA **不需要**；ARENA_GUARD **需要**
   `vm.unprivileged_userfaultfd=1` 或 CAP_SYS_PTRACE（sudo/root）；默认
   sysctl=0 时自动降级为无 guard ARENA（WARN）。
- **三者不可兼得**：快（340ns）+ 省 VMA（~28）靠 ARENA；保护只有 guard
  有；UFFD guard 要特权且慢。→ **release=ARENA、debug/CI=ARENA_GUARD，
  100K+ 且必须安全 → stackless**。

---

## 1. 问题定义与目标

### 1.1 PEL 现状（外部事实，来自 PEL 文档）

| 事实 | 出处 |
|---|---|
| 唤醒链唯一漏斗：`pel_scheduler_wakeup` + loop park；transport = `uv_async_send`（eventfd+epoll，libuv pending 合并） | PEL doc151 §4.3 / doc155 |
| UDP echo 往返 ~11.5µs ≈ **4 对 park/wake**（≈2.9µs/对） | PEL doc156 |
| blocking echo 每请求 2 次完整阻塞原语 + 栈切换 | PEL doc148 D9 |
| 终态矩阵：echo c=1 57.5–69.5K rps/p50 13.0µs（eventfd/req 1.0）；http c=1 47.8K/18.5µs（eventfd/req 2.0）；c=16 110–124K；c=256 ~108K | PEL doc151 §4.3 / doc148 |
| 对 rust c≥16 残差 0.37–0.69× | PEL doc148 D9 |
| stackman switch ~15ns；vstack 创建 5–10µs、8MB VA/fibre | PEL doc22/23 |
| 已否决/收口项：D5 定向投递、D7 recv 常驻读、D8 回调内直投（帧寿命） | PEL doc148 |

### 1.2 xruntime 的残差归因（S1–S9 结论）

1. **transport 是唤醒链主项**：跨线程每对 ~3µs 中 eventfd+epoll 往返是大头；
   registry 查找（V4a）无收益，同线程队列（V3）只省 45% 且 PEL 不可用（D8）。
2. **每请求 park 次数是 c=1 的主成本**：PEL 4 对/往返 ≈ 全部 11.5µs；
   合成 echo 中 driver park 次数 1/req→1/8/req 使吞吐 +4~8×（S9）。
3. **栈模型（stackful/stackless）不否决机制结论**：每次切换 ±30–60ns、
   跨线程 V0 +1.4~2.0%、每请求 park 次数不变（S7/S8）；stackful 的代价在
   创建（51×）、VMA（默认上限 ~3.27 万并发）、RSS（100K 驻留 422MB）。

### 1.3 修改目标

- **M1（机制）**：把每次 park 的成本从 eventfd+epoll（~3µs）降到 futex
  （~0.4µs），保持唯一漏斗与现有一致性契约。
- **M2（API/事件面）**：把每请求 park 次数从 4 降到 1–2（try-before-park、
  write 快路径、批量提交/等待）。
- **M3（可选）**：io_uring 统一 submit+wait 与跨线程唤醒（MSG_RING）。

三者正交，可独立评估；M1 与 M2 不互斥，叠加才是对 rust 差距的正解。

---

## 2. M1：futex transport（增量收敛点）

### 2.1 改什么（行为级，非代码）

在唤醒链**唯一漏斗**内增加 futex 分支，与 eventfd 并存：

1. **睡眠字协议**（单字 `fut_word`）：
   - `0 = 声明睡眠`（仅此刻生产者会 `futex_wake`）；`1 = awake/有唤醒在途`；
   - 等待者睡眠前：`store(0, release)` → **复核**队列/条件 → `futex_wait`；
   - 生产者：`exchange(1, acq_rel)`，**旧值为 0 才** `futex_wake`（latch
     合并，避免饱和形态每次 deliver 一个 syscall）。
2. **路由判据**：按**目标线程当前睡眠原语**选择 transport（睡 futex→
   `futex_wake`；睡 epoll→eventfd），与"本次唤醒有无 IO"无关。
3. **配置期切换**：切换 flags 时 eventfd+futex **双写**，覆盖"线程仍睡在
   旧原语"的窗口；热路径无此开销。
4. **门控契约**：futex 自带单字门控，**不要**再叠加 sleeping/GATE 门控
   （两者互斥：GATE 依赖 mutex 队列串行化）。
5. **组合正确性**：`FUTEX|MPSC` 正确（单字协议不依赖队列串行化）；
   GATE 在 `MPSC|FUTEX` 下必须自动失效，否则丢唤醒。

### 2.2 为什么可行（沙盒证据）

| 实验（同机 release，3 轮中位） | V0 | V5 futex | Δ |
|---|---|---|---|
| B1 跨线程每对 RTT | 3.0–3.2µs | **390ns** | **−88%** |
| B1 同线程 RTT | 50ns | 20ns（V3 MPSC） | −45%（仅同线程） |
| B2 wait（请求-响应） | 1.99 Mops/s | 4.62 Mops/s | **+132%** |
| B2 unpaced（饱和） | — | — | **−11%**（延迟换吞吐） |
| B3 spin M=1 | 285K rps | 2.01M rps | **+602%**，p50 3336→430ns |
| B3 park M=4（S8 修正） | 0.62–0.68M | 0.72–0.90M | **+15~40%**，p50 ~5.5→4.5µs |

- **PEL 侧含义**：PEL 每对 ~2.9µs 与沙盒 V0 同量级，说明唤醒链本身没有额外
  浪费；把 eventfd+epoll 换成 futex 的差值可直接对标沙盒 −88%（但高并发
  transport 已被 libuv pending 合并，见 §4）。
- **不建议回灌的部分**：V3 MPSC 只对同线程 hop 有效（PEL 不可能把工作搬回
  同线程，D8 已否决）；V4a 无收益。

### 2.3 预期收益（沙盒外推，**已被 §0.3 的 PEL 实测修正**）

> 下表是"唤醒为跨线程"假设下的外推；PEL 实测 self-wake ~100%，**M1 现有
> 形态 ROI≈0**。保留本表仅用于展示外推口径与偏差来源。

| PEL 场景 | 现状 | M1 预估（旧外推，已修正） | 依据 |
|---|---|---|---|
| echo c=1 | 57.5–69.5K，p50 13.0µs（eventfd/req 1.0） | ~~+10~30%~~ → **≈0**（self-wake） | 每请求 1 次真 transport 假设不成立 |
| http c=1 | 47.8K，p50 18.5µs（eventfd/req 2.0） | ~~+15~35%~~ → **≈0** | 同上 |
| echo/http c=16 | 110–124K（0.38/0.13 次写/req） | **+3~10%** | transport 大量合并（PEL 实测 0.076/req） |
| c=256 | ~108K（0.008–0.025/req） | **基本持平** | 瓶颈不在唤醒（PEL 实测 0.0049/req） |
| UDP echo | 4 对/往返 11.5µs | **最多 +20~40%** | 取决于"真跨线程且真睡着"的对数（未测） |

### 2.4 风险与回滚

- **hybrid 竞态**：同线程 loop 既要服务 futex 又要服务 epoll；"决定睡
  futex vs epoll"存在窗口，必须与现有 pending/门控协议合并评审。
- **D8 边界**：M1 不消 self-wake（那是 D8 帧寿命议题，另案）；形态 B 是否
  触犯 D8 需评审。
- **丢唤醒**：单字协议的正确性依赖"先声明睡眠再复核 + 0→1 才 wake"；
  测试清单见 §5.3。
- **回滚**：保留 eventfd 路径，配置开关（flags）切换即可退回；M1 不删旧
  路径、不改队列/registry/lifetime。

---

## 3. M2：park 次数削减（API/事件面）

### 3.1 手段与对应关系

| 手段 | 消掉什么 | 沙盒对应证据 |
|---|---|---|
| **try-before-park**：recv/send/accept 先非阻塞试一次，EAGAIN 才 park | "数据已到/可写"时的整个 park | B3 spin（driver 不 park）vs park：**≈2×** |
| **write 快路径**：小响应 copy+flush，队列满才 park | 请求-响应 send 半边的 park | PEL D9：每请求 2 次完整阻塞原语 |
| **批量提交/等待**：在途窗口 B，每 B 个完成 park 一次 | driver 侧 1/B 的 park | U9 曲线（下表） |
| **loop 侧 drain**：一次事件处理同一 fibre 的多个就绪工作 | 合并唤醒次数 | B2/B3 合并率：并发↑→transport/req↓ |
| **io_uring**（M3） | submit+wait 的 syscall 与事件面 park | 沙盒未测（门禁项，见 V9.1） |

> **PEL 实测修正（§0.2）**：c=1 即时消费仅 **0.38%** → try-before-park 在
> 同调用数下**无靶**；M2 的有效手段是**减少阻塞调用数**（send 快路径/
> 批量提交），且受 D9 用户裁决约束（libuv 写完成回调是完成确认点，禁止
> tokio 式纯异步写）。

### 3.2 U9 曲线（合成 echo，K=16，M=4，V5，stackless）

| B（在途窗口） | rps | p50 | p99 | driver-parks/req | futex/req |
|---|---|---|---|---|---|
| 1（旧口径） | 0.82M | 4.5µs | 20µs | 1.00 | 0.50 |
| 4 | 3.2–3.4M | 4.0–4.3µs | 24µs | 0.25 | 0.12 |
| 8 | 6.2–6.4M | 4.2µs | 21µs | 0.12 | 0.04–0.06 |
| 64 | 6.2–10.1M（方差大） | 4.4–5.4µs | 154–697µs | 0.002–0.003 | 0.004–0.011 |

- **吞吐与 park 次数近似反比**：1→1/8 给 **+4~8×**；p50 恒定（完成延迟
  不退化），代价全在 p99 批尾；B≥8 后受剩余 conn 唤醒/队列成本限。
- L0 无关：fibre B=1 0.81M、B=8 5.7–7.1M，同曲线。
- transport 仍可见：eventfd B=8 3.8–5.5M vs futex B=8 6.2–6.4M。
- **不可外推 PEL（§0.3）**：该曲线削减的是**跨 worker park**；PEL 是同一
  loop 线程 self-wake + 阻塞调用数，二者形状不同。

### 3.3 预期收益与边界（已被 §0.3 修正）

- ~~PEL UDP echo 4 对/往返……4 对→2 对 ≈ 吞吐翻倍~~：PEL 实测 c=1 挂起
  2.11–2.4/req、即时消费 0.38%，**该外推不成立**；M2/self-wake 的总上界
  = **c=1 +20%**（doc165 §2.2，PEL 额外 2.1µs/req）。
- **不能把对 rust 的 0.37–0.69× 拉到 1.0× 的保证**：每 op 固定机器成本
  （parker claim、队列、栈切换）仍在；高并发差距按 doc166 归因走观测面+
  阻塞机件（合计 ~5–7% → 0.80~0.83×），不是唤醒。
- 代价：try-first 无靶（见 §0.2）；write 快路径改变背压/错误时序且受 D9
  约束；批量改变完成粒度与 API 语义。

### 3.4 实施顺序建议（已被 §0.5 取代，仅留档）

1. 先量 PEL 现状：每请求 park 次数、EAGAIN 比例、transport/req
   （插桩计数 + `strace -c -f -e trace=futex,write,epoll_wait,read,recvfrom`）；
2. try-before-park（改动最小、语义不变，优先）；
3. write 快路径（需要设计背压策略）；
4. 批量等待（改 API，放最后，需要产品确认完成粒度语义）。

---

## 4. 组合预期与天花板（按 §0 修正）

- **机制地板**（沙盒实测）：同线程 20ns/对、跨线程 390ns/对（futex）。
- **PEL 实测口径**（doc165）：c=1 挂起 2.11–2.4/req、eventfd 写 1.21/req
  （self-wake）、EAGAIN=0、即时消费 0.38%；c=16 挂起 1.21、写 0.076；
  c=256 写 0.0049。PEL vs libuv-raw 同窗：c=1 0.82、c=16 0.76、c=256 0.64。
- **组合矩阵（修正后）**：

| 组合 | 每次唤醒成本 | 每请求挂起数 | c=1 | c≥16 |
|---|---|---|---|---|
| 现状 | self-wake eventfd | 2.1–2.4 | 基准（vs uv 0.82） | 基准（0.64–0.76） |
| M1 futex | futex | 同 | **≈0（self-wake）** | ≈0（已合并） |
| M2 减调用数 | self-wake | 2.4→?（受 D9） | 上界 **+20%** | 需实测 |
| doc166 P1+P2 | 同 | 同 | 小 | **+5~7% → 0.80~0.83×** |
| P3 多 worker 分发 | 同 | 同 | — | 能力课题（多线程对标） |
| M3/形态 C | futex/io_uring | 1–2 | 事件面重构，超出本设计 | 更大课题 |

---

## 5. 验证计划与判据

### 5.1 同窗单变量 A/B（PEL 侧）

> **前提（§0）**：现有请求形态 M1 无靶（self-wake ~100%）；本节 A/B 仅适用
> 于"真跨线程通知"场景或形态 B（idle-loop futex）评审立项之后。

- 基线：PEL 终态（D4+D7+D6），同机同窗、绑核、governor 固定（先切
  performance 或全程 powersave 并记录）；
- 变量：**只换 transport**（M1）→ 跑 echo/http c=1/16/256 + UDP echo；
- 指标：rps、p50/p99、`eventfd/req`、`futex/req`、runs/req、
  `strace -c`、`perf stat`（cycles/instructions/cache-misses）；
- **判据**：≥5% 或分布明确前移才保留；否则回滚。M2 各自独立 A/B。

### 5.2 复现命令（xruntime 沙盒，供评审复算）

```bash
./scripts/build.sh release
./scripts/run-bench.sh release bench_roundtrip --flags 0x20
./scripts/run-bench.sh release bench_echo --conns 16 --drivers 4 --ops 20000 \
    --ack=park --flags 0x20 --batch 1   # 与 --batch 4/8/64 对照
./scripts/run-bench.sh release bench_l0 --mode calib --ops 5000000
./scripts/run-bench.sh release bench_l0 --mode ring --l0 fibre --fibres 100000 \
    --ops 2000000 --stack 16384
sudo perf stat -e dtlb-loads,dtlb-load-misses,cycles,instructions -- \
    ./build-release/bench/bench_l0 --mode ring --l0 fibre --fibres 100000 \
    --ops 2000000 --stack 16384
```

### 5.3 正确性测试清单（M1 必须）

- parker 三态单测：CONSUMED/SUSPENDED、STORED/MERGED/DELIVER；
- 跨线程 1000 并发 unpark 仅 1 次 DELIVER（`tests/test_parker.c` 已有）；
- futex 单字协议并发：饱和 deliver 下 0→1 latch 无丢唤醒；
- 配置期切换双写：切换窗口内不丢唤醒；
- `MPSC|FUTEX`、`GATE` 自动失效契约；
- 停止/销毁：parked 任务清理（L0 生命周期元规则，00 §2）；
- sanitizer：asan/tsan/ubsan 按 PEL 门禁（自定义栈与 ASan/TSan 不兼容时，
  L0 测试单独豁免并记录）。

---

## 6. 数据可信度与偏差

| 偏差 | 影响 | 方向 |
|---|---|---|
| governor=powersave 未切换 | 跨 session 不可比；只信同窗 | 双向噪声 |
| 沙盒 stackless、无 libuv | 每对成本不含 PEL 的 loop tick/回调 | 低估 PEL 收益或高估 |
| 自研最小 asm 5.6ns（PEL stackman ~15ns） | stackful 成本被低估 | 保守（对 stackful 有利） |
| mmap+guard 栈，非 vstack slot pool/madvise | VMA/TLB/RSS 低于 PEL 8MB VA/fibre | stackful 成本为**下限** |
| perf 介入使绝对延迟 ~2× | 只做相对比较 | 相对可信 |
| 合成 echo 非真实 IO | 不含内核收发/协议栈成本 | 只用于机制归因 |
| S6 park 数字曾含 bench bug（S8 已修正） | 旧 +32~43% → 修正 +15~40% | 已按修正值改写 |

---

## 7. 开放问题（2026-10-03 更新）

已闭环（doc165/166）：

1. ~~PEL 实际每请求 park 次数~~ **已测**：c=1 挂起 2.11–2.4/req、eventfd 写
   1.21/req（self-wake）；c=16 1.21/0.076；c=256 1.18/0.0049；
2. ~~EAGAIN 比例~~ **已测 = 0**；即时消费 c=1 0.38%、c=16 ~50%（send 半边）
   → try-before-park 无靶，M2 转"减少阻塞调用数"；
3. ~~跨线程唤醒占比~~ **已测 ≈0**（self-wake ~100%）→ M1 现有形态 ROI≈0；
4. 高并发归因 **已测（doc166）**：集中用户态观测面 ~3.8% + 阻塞机件 ~6.4%，
   非弥散模型税。

仍未闭环：

5. **阻塞机件 ~6.4% 的行级下钻**（doc166 §3.1b 下批：单点定位 + 单变量 A/B，
   预期 +3%）；
6. **多 worker 分发**（per-connection migrate / per-channel listener）与
   SO_REUSEPORT 可行性（多线程对标能力课题）；**U11（04 §3.9）**：迁移在
   切换层便宜（57ns/周期），成本在在途注册重绑与 TLS 契约 → 优先 accept
   时分发，迁移只作再平衡；
7. hybrid 路由（形态 B）的"无 IO"窗口判定与丢唤醒注入测试；形态 B 与
   D8 帧寿命红线的关系；
8. vstack 真实 VMA/RSS/TLB（沙盒数为下限；默认 `vm.max_map_count=65530`
   → ~3.27 万并发上限是否影响 PEL 目标形态）；
9. io_uring 可用性（内核 6.6.13 本机可用）与形态 C/事件面替代面；
10. M2 的 API 语义（背压、完成粒度、错误时序）产品确认（受 D9 约束）。

---

## 8. 附录

### A. 术语

- **notify / transport / resume 三分**（02 §V9.2）：notify=任务就绪+入队；
  transport=仅目标线程确在睡时按其原语打断；resume=owner 恢复执行。
- **每对 park/wake**：一次阻塞操作（park→被唤醒）的完整周期。
- **park 次数/请求**：请求路径上所有阻塞操作数（PEL blocking echo 现状
  2 次完整阻塞原语；UDP echo 4 对/往返）。

### B. futex 单字协议伪码

```c
/* 等待者(唯一 park 侧) */
atomic_store(&fut_word, 0, release);
if (queue_nonempty()) { atomic_store(&fut_word, 1, release); continue; }
syscall(SYS_futex, &fut_word, FUTEX_WAIT_PRIVATE, /*val=*/0, timeout, ...);
atomic_store(&fut_word, 1, release);

/* 生产者(多 unpark 侧) */
if (atomic_exchange(&fut_word, 1, acq_rel) == 0)
    syscall(SYS_futex, &fut_word, FUTEX_WAKE_PRIVATE, 1, ...);
```

### C. 证据索引（xruntime）

- 文档：`00-unified-constraints.md`（约束/契约）、`01-plan.md`（S1–S9 台账）、
  `02-upper-bound-design.md`（§0.5 L0、§V9.1/§V9.2）、
  `04-final-report.md`（§0–§3.7）、`05-measurement-toolbox.md`（方法）。
- 日志：`bench-logs/bench_roundtrip-20261003-0240*`（L0）、
  `bench-logs/bench_echo-20261003-0310*`（修正 park）、
  `bench-logs/bench_l0-20261003-0310*`（ring/TLB）、
  `bench-logs/perf-tlb-20261003-031138.log`、
  `bench-logs/bench_echo-20261003-0341*`（U9 曲线）。

### D. PEL 侧对应锚点

- 唤醒链：`pel_scheduler_wakeup`、loop park（`src/eventloop/`）；
- parker 契约：`src/scheduler/pel_parker.h`、PEL 00 §5.2 / doc97；
- 评审门禁：doc155 §7.7；终态矩阵：doc151 §4.3；模型税：doc148 D9；
- **实测裁定**：doc165（M1 futex 设计 + eBPF：self-wake ~100%、挂起/写数、
  EAGAIN=0、即时消费、PEL vs uv 上界）、doc166（高并发归因：观测面 ~3.8% +
  阻塞机件 ~6.4%；观测面批已落地 −2.2pt/+1.0%）、doc163（观测面税前作）；
- 证据：`test-logs/ebpf-park-*.txt`、`perf-results-cross/report-20261003-*.md`；
- 栈/内存：doc22/23（stackman/vstack）。
