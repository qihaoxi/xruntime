# xruntime 04 — 总报告:S1–S6 与决策建议

日期:2026-10-03 · 状态:终稿 · 上游:`docs/01-plan.md`(计划/判据/台账)、
`docs/00-unified-constraints.md`(约束)、`docs/02-upper-bound-design.md`(上限路径)、
`docs/03-handoff-s4-s6.md`(实施交接)

> 证据纪律:全部数字为**同机同窗 release、3 轮 median**(governor=powersave
> 未切换,只信 ≥5% 或分布明确前移);原始日志在 `bench-logs/`、`test-logs/`。

## 0. TL;DR

1. **PEL 残差确在 wake 机制,但主项是 transport,不是队列/registry**:跨线程
   每对 park/wake 的 ~3µs 里,eventfd+epoll 往返是大头;换 futex 单字协议后
   **RTT 3196→390ns(-88%)**。同线程 hop 的固定税是 mutex 队列
   (**50→20ns,-45%**,V3)。registry 查找(V4a)无收益。
2. **超 V0 ≥5% 的变体只有两个**:
   - **V5 futex transport**:B1 cross -88%、B2 wait +132%、B3 echo(1 worker,
     M=1)+602%、park 口径 **+15~40%(S8 修正;旧 +32~43% 含 bench_echo
     drv_step 通知双消费 bug)**;唯一回退是 B2 unpaced **-11%**(即时唤醒
     降低合并度,延迟换吞吐);
   - **V3 MPSC 队列**:仅同线程 hop -45%,跨线程/B2 噪声内。
3. **迁移建议**:transport 一项值得以"增量收敛点"回灌 PEL(候选形态:调度器
   唤醒链唯一漏斗内做 futex/eventfd 双形态,libuv 侧走 hybrid);队列/lifetime
   变体无收益不迁移。**另立运行时的必要性未获证明**:机制地板已测出
   (同线程 20ns、跨线程 390ns/对),剩余对 rust 的差距更可能在 stackful 模型
   与事件面形态(需 L0 对照轮,见 01 §0.5),而非唤醒链。
   **L0 对照轮已做(§3.6)**:每次切换模型税 ±30–60ns,跨线程 V0 +1.4~2.0%、
   V5 排序不变 → 不被栈模型否决;量级差只剩高频创建/销毁(51×)与未测 TLB。

## 1. 问题一:PEL 残差是否在 wake 机制?

| 证据 | 数字 | 结论 |
|---|---|---|
| S3 同线程 hop vs 跨线程 RTT | 50ns vs 3.0µs(50×) | 直投只在同线程有空间 |
| S3 V1(直投)/V2(门控) | 全噪声内 | 同线程 transport 已被 pending 合并消掉;门控无靶 |
| S4 V4a(去 registry) | cross +1.6%、B2 噪声 | **registry 查找不是固定税**(单桶) |
| S4 V3(MPSC) | 同线程 RTT 50→20ns(-45%);cross/B2 噪声 | 同线程固定税=mutex 队列 |
| S5 V5(futex) | cross RTT 3196→390ns(-88%);B2 wait +132% | **transport 是跨线程主项** |
| S5 V5 unpaced | -11% | 即时唤醒降低合并度(延迟↔吞吐) |
| S6 B3 spin M=1 | 285K→2.01M rps(+602%),p50 3336→430ns | 低并发(每请求 ~1 transport)收益最大 |
| S6 B3 park M=4 | 0.62~0.68M→0.72~0.90M rps(+15~40%),p50 ~5.4–6.5→4.2–5.5µs | 请求-响应(2 对/请求,futex/req≈0.50)口径同向;S8 修正 |

**分段归因(本机)**:
- 同线程一对:2 RMW + 队列 ~20ns(V3 后);
- 跨线程一对:cache line 转移 + 唤醒原语;eventfd+epoll ≈ 1.5–2.9µs,
  futex ≈ 0.2–0.4µs(未睡零 syscall);
- 每请求 transport 次数随并发合并下降(B3 transport/req:M=1 ~1.0 →
  M=4 ~0.04–0.25 → M=16 ~0.04–0.05),故低并发是 transport 敏感区。

**边界**:沙盒默认 stackless;L0 对照轮(§3.6)已补 stackful 后端。PEL 每对
~2.9µs 与 V0 同量级说明 PEL 的唤醒链本身没有额外浪费,差距在模型与事件面。

## 2. 问题二:哪个变体在何规模超 V0 ≥5%?

| 变体 | 口径 | 收益 | 判定 |
|---|---|---|---|
| V1 DIRECT | B1 same/cross | 噪声 | 不保留默认 |
| V2 GATE | B1/B2 | 噪声 | 不保留默认 |
| V3 MPSC | B1 same-thread | RTT p50 -45% | 保留 flag;跨线程无感 |
| V4a WAKER_DIRECT | B1/B2 | 噪声(略负) | 保留 flag;H3 证伪 |
| **V5 FUTEX** | B1 cross / B2 wait / B3 | **-88% / +132% / +40~602%** | 保留 flag;低并发必选 |
| V5 FUTEX | B2 unpaced | **-11%** | 饱和形态回退(延迟换吞吐) |

规模判据:
- **低并发(M=1)/阻塞请求-响应**:futex 决定性(-88% RTT,+6× rps);
- **中并发(M=4)**:+8%(spin)/**+15~40%(park,S8 修正口径)**;
- **高并发饱和(M=16, unpaced)**:futex 中性偏正(spin M=16 中位 +18%,
  噪声大),但 B2 unpaced -11%——**transport 与合并度此消彼长**;
- **等待粒度(U9)**:driver 在途窗口 B=8 → **+4~8×**(p50 恒定,p99 批尾);
  park 次数与吞吐近似反比,是 c=1 的主成本(§3.7);
- 同线程:futex 无差异(不睡眠路径),MPSC 队列 -45%。

## 3. 问题三:回灌 PEL 还是另立运行时?

**可回灌(增量收敛点)**:
- **futex 唤醒原语**:价值最高、改动面最小(唤醒链已有唯一漏斗:
  PEL `pel_scheduler_wakeup` + loop park)。形态建议:worker/loop 睡眠用
  futex 单字协议(0=声明睡眠/1=awake,0→1 才 `futex_wake`),真实 IO 仍走
  epoll;两形态按**目标线程当前睡眠原语**路由(睡 futex→futex_wake;
  睡 epoll→eventfd;与"本次唤醒有无 IO"无关)。风险:libuv 集成需 hybrid
  (不能只 futex 不 epoll),须按 PEL 核心目录门禁走设计+评审。
  > 注:此 **hybrid 是"同线程 loop + stackful"的妥协**——一个线程必须同时
  > 服务两类唤醒。上限路径按 **tokio 式角色分层**(worker 睡 futex、
  > 选举 driver 睡 epoll、eventfd 仅控制面);概念拆解见
  > `02-upper-bound-design.md` §V9.2(notify / transport / resume 三分)。
- **MPSC 队列**:只对同线程 hop 有意义,收益 -45% 但 PEL 瓶颈不在同线程
  队列,优先级低。

**不迁移**:
- V1/V2(无靶)、V4a(registry 查找非税)、V4b(生命期引用计数,无证据需求)。

**另立运行时**:
- 机制地板:同线程 20ns/对、跨线程 390ns/对、阻塞 echo 单 worker
  1.6–1.8M rps(park 口径)。若新运行时以 stackless + futex + LIFO slot +
  内嵌 waker 为目标,理论唤醒段成本已在此量级;
- 但**没有证据表明**唤醒机制能解释 PEL 对 rust c≥16 的 0.37–0.69×:该差距
  更可能来自 stackful 模型税与事件面形态(recv 常驻读已收口、transport 已
  被 libuv 合并)。**L0 栈切换对照轮已做(§3.6)**:每次切换税 ±30–60ns、
  跨线程 V0 +1.4~2.0%、V5 排序不变 → 机制结论不被栈模型否决;另立运行时
  的必要性仍集中在"高频创建/销毁(51×)+ TLB(未测)"与编程模型,不在唤醒链。

### 3.5 回灌收益预估与验证计划

> 性质:**工程预估(待 PEL 同窗 A/B 确认),非承诺**。沙盒量的是"唤醒对"本身;
> PEL 每对还叠加 registry/锁/队列/栈切换/loop tick,且高并发下 transport 早已
> 被 libuv pending 合并——**不能按沙盒 -88% 直接外推**。

| 场景 | PEL 现状(doc151 §4.3/终态矩阵) | hybrid 回灌预估 | 依据 |
|---|---|---|---|
| echo c=1 | 57.5–69.5K rps,p50 13.0µs(eventfd/req 1.0) | **+10~30%,p50 ~10–12µs** | 每请求 1 次真 transport;沙盒 eventfd+epoll 3.0µs→futex 0.39µs |
| http c=1 | 47.8K,p50 18.5µs(eventfd/req **2.0**) | **+15~35%,p50 ~14–16µs** | 每请求 2 次,省得更多 |
| echo/http c=16 | 110–124K(0.38/0.13 次写/req) | **+3~10%(可能在噪声内)** | transport 大量合并;loop 多在 epoll 等服务 IO,futex 只在"无 fd 等待窗口"生效 |
| c=256 | ~108K(0.008–0.025/req) | **基本持平** | transport 已摊薄;瓶颈在调度/事件面不在唤醒 |
| UDP echo | 4 对/往返 11.5µs | **最多 +20~40%**,取决于其中多少对是"跨线程且真睡着" | 4 对里同线程 IO 回调占多数,hybrid 抓不到 |

**为什么是区间**:PEL 是同线程 loop + stackful,futex 只能覆盖"loop 无 fd 可等、
睡在 futex"的窗口;loop 睡在 epoll(IO 服务态)时跨线程唤醒仍须 eventfd。
沙盒把"任务就绪"与"线程睡眠"拆开了,所以 -88% 不可搬。

**上限与边界**:
- 回灌(hybrid)现实上限:低并发延迟 -10~35%、高并发 +3~10%;
  **不可能把对 rust 的 0.37–0.69× 拉到 1.0×**——那是 stackful 模型税
  (每请求两次完整阻塞原语 + 栈切换,D9 定性);
- tokio 式角色分层在 PEL 的 stackful 同线程回调模型下会给每个 IO 加一次
  跨线程 handoff(正是要消灭的跳数)→ 不划算;**吃满收益需另立运行时**
  (stackless + futex + LIFO + 可选 io_uring;地板见上);
- 高并发不动的部分:V4a 证明 registry 非税、V3 只对同线程 hop 有效(PEL
  不可能把工作搬回同线程,D8 已否决)→ 回灌只盯 transport 一处。

**验证计划(立项判据)**:同窗单变量 A/B——PEL 终态(D4+D7+D6)vs 只换
transport;echo/http c=1/16/256 + UDP echo;记录 p50/p99、`eventfd/req`、
`strace -c`、perf;≥5% 或分布明确前移才保留。走 PEL 核心目录设计评审
(doc155 §7.7),沙盒结论不构成修改依据。
**完整评估材料**(M1 futex 回灌 / M2 park 次数削减 / M3 io_uring 的步骤、
原因、数据、组合预期、验证清单、开放问题)见
`docs/06-pel-modification-eval.md`。

### 3.6 L0 栈切换对照轮结果(U7,2026-10-03)

> 仪器:自研最小 6 push/pop asm(`xr_switch_x86_64.S`,标定 **5.6ns/switch**,
> 快于 PEL stackman ~15ns→结论保守);mmap+guard 栈(非 vstack slot pool,
> VA/TLB 压力低于 PEL);完整偏差与判据见 02 §0.5.5、01 S7。

| B1(release,ops=200k) | stackless p50 | fibre p50 | Δ |
|---|---|---|---|
| same-thread V0 / V5 | 20ns | 50ns | +30ns(微口径) |
| cross-thread V0 | 2.93–2.95µs | 2.99–3.01µs | **+1.4~2.0%** |
| cross-thread V5 | 330ns | 360ns | +30ns(+9%) |

- **V5 增益不变**:stackless −88.8% / fibre −88.0%,变体排序与 unpark 分布
  不变 → **机制结论可回灌 stackful PEL,不被栈模型否决**;
- **创建/销毁**:141ns vs 7.20µs/个(**51×**,mmap/mprotect/munmap);
  高频任务创建(per-request)形态下这是唯一量级差,PEL 侧靠 vstack slot
  pool 复用摊薄,需要时再以同窗 A/B 验证;
- **驻留/TLB/VMA(S8 补测,N 驻留节点 token-ring,2M 跳)**:
  - rtt p50(µs):stackless 3.33~3.38(N=16~100K 平);fibre 3.37(N=16)→
    **3.52(N=100K)**,consumer p50 1.96→2.18µs(+11%);
  - perf TLB:fibre N=16 miss 2.9%(0.92M/32.1M loads)→N=100K **13.3%**
    (29.5M/221M);stackless N=100K 12.5%(24.3M/194M)——差 ~2.6 misses/hop,
    **TLB 是工作集效应非栈页专有**,rtt 差 ≤1%(perf 口径);
  - **VMA 硬墙(默认约束)**:vm.max_map_count=65530,每 fibre 2 VMA →
    N≈32.7K 即 mmap 失败;放宽到 1M 后才跑通 N=65K/100K。stackless 无此限;
  - RSS:N=100K 驻留 fibre **422MB**(VmPeak 2.1GB)vs stackless 123MB;
- **未测**:真实 PEL vstack(slot pool/madvise/零页语义)下的 VMA/TLB/RSS,
  本次为下限;高频 per-request 创建形态未建 bench。

### 3.7 park 次数削减曲线(U9,2026-10-03)

> 问题:每请求 park 次数是否是最大成本?`bench_echo --batch B` 让 driver
> 每 B 个完成才 park 一次(在途窗口 B),同机制/同 L0,只改等待粒度。

| B(K=16,M=4,V5,stackless) | rps | p50 | p99 | driver-parks/req | futex/req |
|---|---|---|---|---|---|
| 1(旧口径) | 0.82M | 4.5µs | 20µs | 1.00 | 0.50 |
| 4 | 3.2–3.4M | 4.0–4.3µs | 24µs | 0.25 | 0.12 |
| 8 | 6.2–6.4M | 4.2µs | 21µs | 0.12 | 0.04–0.06 |
| 64 | 6.2–10.1M(方差大) | 4.4–5.4µs | 154–697µs | 0.002–0.003 | 0.004–0.011 |

- **吞吐随 park 次数近似反比**:1→1/8 → **+4~8×**;p50 恒定(完成延迟
  不退化),p99 随 B 增长(B=64 批尾);B≥8 后受剩余 conn 唤醒/队列成本限;
- **L0 无关**:fibre B=1 0.81M、B=8 5.7–7.1M,与 stackless 同曲线;
- transport 仍可见:eventfd B=8 3.8–5.5M vs futex B=8 6.2–6.4M;
- **结论**:每请求 park 次数是 c=1 的主成本,且可由"等待粒度"削减——
  对 PEL 的对应手段 = **try-before-park**(消 ready 侧 park)+ **write
  快路径**(消 send park)+ **批量提交/等待** + **io_uring**(一次 enter
  管 N op);代价是 p99 批尾与 API 语义(背压/完成粒度)变化,B=4~8 是
  延迟/吞吐折中点。

## 4. 执行摘要与证据位置

| 阶段 | 结论 | 日志 |
|---|---|---|
| S1/S2 | 骨架/V0 基线(B1 RTT 3.0µs、B2 M=1 0.995) | `bench-logs/bench_{env,roundtrip,fanin}-20261002-*` |
| S3 | V1/V2 噪声内(同线程 60ns vs 跨线程 3µs) | 同上 + `01-plan` S3 验证 |
| S4 | V4a 噪声;V3 同线程 -45%;GATE×MPSC 丢唤醒修复 | `bench-logs/bench_{roundtrip,fanin}-20261003-00[01]*` |
| S5 | futex -88%/+132%/-11%;单字协议契约 | `bench-logs/bench_{roundtrip,fanin}-20261003-0019~0026*` |
| S6 | B3 echo:spin M=1 +602%、park +15~40%(S8 修正) | `bench-logs/bench_echo-20261003-*` |
| S7 | L0 对照:cross V0 +1.4~2.0%、V5 排序不变、创建 51×、切换 5.6ns | `bench-logs/bench_{roundtrip,l0}-20261003-0240*` |
| S8 | park 口径 bug 修正 + fibre echo 对照 + ring 驻留/TLB/VMA | `bench-logs/bench_{echo,l0}-20261003-0310*`、`perf-tlb-20261003-031138.log` |
| S9 | park 次数削减曲线:B=1→8 吞吐 +4~8×、p50 恒定、parks/req 1→0.12 | `bench-logs/bench_echo-20261003-0341*` |

## 5. 风险与边界

1. 沙盒默认 stackless;S7 补了 stackful 对照(自研 asm+简单 mmap 栈),
   不含 PEL vstack 的 madvise/slot pool/TLB 税,stackful 成本是**下限**,
   结论不能直接等同 PEL;**S8 补:** 每 fibre 2 VMA,默认 max_map_count=
   65530 → N≈32.7K 并发即 mmap 失败;100K 驻留 RSS 422MB(vs stackless
   123MB)。若目标形态含大规模并发驻留,VMA/RSS 先于 TLB 成为硬约束;
2. governor=powersave 未切换:B3 M=16 方差大,只取中位与方向;跨 session
   数字不可比;
3. B3 单 worker 绝对 rps 与 PEL 多 channel/rust 公开数**不可比**,只作形态
   参照(随 K/M 的拐点与合并率趋势);
4. 回灌 PEL 必须重新评审(PEL 00 §0.3.2 + doc155 §7.7),本报告不构成对
   PEL 核心目录的修改依据;
5. TSan 单次 test 栈对象报告(裸 futex 非拦截路径)28 次不复现,留档待复现。
