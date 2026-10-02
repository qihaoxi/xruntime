# xruntime 06 — PEL 修改评估文档：transport 回灌 + park 次数削减

日期：2026-10-03 · 状态：提案（待 PEL 核心目录评审）· 证据：xruntime S1–S9
沙盒同窗实测（`bench-logs/`、`test-logs/`）

> **性质声明**：本文是**评估材料**，不是修改授权。所有沙盒数字为
> stackless、无 libuv、governor=powersave 的**相对值**；回灌 PEL 必须走
> PEL 核心目录设计+评审门禁（doc155 §7.7 反绕过纪律），并以 PEL 侧同窗
> 单变量 A/B 为最终判据。沙盒结论不构成对 PEL 任何目录的修改依据。

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

### 2.3 预期收益（分场景，待 PEL 同窗 A/B 确认）

| PEL 场景 | 现状 | M1 预估 | 依据 |
|---|---|---|---|
| echo c=1 | 57.5–69.5K，p50 13.0µs（eventfd/req 1.0） | **+10~30%，p50 ~10–12µs** | 每请求 1 次真 transport |
| http c=1 | 47.8K，p50 18.5µs（eventfd/req 2.0） | **+15~35%，p50 ~14–16µs** | 每请求 2 次，省得更多 |
| echo/http c=16 | 110–124K（0.38/0.13 次写/req） | **+3~10%（可能在噪声内）** | transport 大量合并 |
| c=256 | ~108K（0.008–0.025/req） | **基本持平** | 瓶颈不在唤醒 |
| UDP echo | 4 对/往返 11.5µs | **最多 +20~40%** | 取决于其中"真睡着"的对数 |

### 2.4 风险与回滚

- **hybrid 竞态**：同线程 loop 既要服务 futex 又要服务 epoll；"决定睡
  futex vs epoll"存在窗口，必须与现有 pending/门控协议合并评审。
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

### 3.3 预期收益与边界

- PEL UDP echo 4 对/往返：若 try-first 消掉"已到"的 recv park、write 快
  路径消掉 send park，最坏省 2 对（~5.8µs）；**4 对→2 对 ≈ 吞吐翻倍**
  （同 spin/park 证据）。
- **不能把对 rust 的 0.37–0.69× 拉到 1.0× 的保证**：每 op 固定机器成本
  （parker claim、队列、栈切换）仍在；M2 只把次数降下来，让 M1 的
  0.4µs/对有意义。
- 代价：try-first 在真会阻塞时多一次 EAGAIN syscall（需先量 EAGAIN 比例）；
  write 快路径改变背压/错误时序；批量改变完成粒度与 API 语义；B=4~8 为
  延迟/吞吐折中。

### 3.4 实施顺序建议

1. 先量 PEL 现状：每请求 park 次数、EAGAIN 比例、transport/req
   （插桩计数 + `strace -c -f -e trace=futex,write,epoll_wait,read,recvfrom`）；
2. try-before-park（改动最小、语义不变，优先）；
3. write 快路径（需要设计背压策略）；
4. 批量等待（改 API，放最后，需要产品确认完成粒度语义）。

---

## 4. 组合预期与天花板

- **机制地板**（沙盒实测）：同线程 20ns/对、跨线程 390ns/对（futex）。
- **PEL 每请求成本模型**：`成本 ≈ park 次数 × 每对成本 + IO/应用/loop`。
  UDP echo 现状 `4 × 2.9µs ≈ 11.5µs`；若 M1+M2 达到 `2 × 0.4–1µs` +
  其余机器成本，理论上限约 1–3µs 级，但实际受 libuv loop、fibre 模型与
  应用处理限制，**必须以 PEL 同窗 A/B 为准**。
- **组合矩阵（评估用）**：

| 组合 | 每次 park | 每请求 park 数 | 低并发 | 高并发 |
|---|---|---|---|---|
| 现状 | ~3µs | 4 | 基准 | 基准 |
| M1 | ~0.4µs | 4 | +10~35% | +3~10% / 持平 |
| M2 | ~3µs | 1–2 | 最多 +2× | 取决于合并度 |
| **M1+M2** | ~0.4µs | 1–2 | 方向叠加，需实测 | 上限受事件面/应用 |
| M1+M2+M3 | ~0.4µs | 1–2 | 事件面 syscall 进一步下降 | 需先量 D7 后剩余 recv 税 |

---

## 5. 验证计划与判据

### 5.1 同窗单变量 A/B（PEL 侧）

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

## 7. 开放问题（需 PEL 侧补测后回填）

1. PEL 实际**每请求 park 次数**分布（插桩计数，按 c 分层）；
2. recv/send 的 **EAGAIN 比例**（决定 try-before-park 的 ROI）；
3. hybrid 路由的竞态窗口实测与丢唤醒测试；
4. vstack 真实 VMA/RSS/TLB（沙盒数为下限；默认 `vm.max_map_count=65530`
   → ~3.27 万并发上限是否影响 PEL 目标形态）；
5. io_uring 可用性（内核版本/seccomp/`kernel.io_uring_disabled`）与
   MSG_RING 对 libuv 的替代面；
6. M2 的 API 语义（背压、完成粒度、错误时序）产品确认；
7. 与 D5/D7/D8 的关系：已否决项不重开；try-first/write 快路径是否触犯
   D8 帧寿命红线，需按新形态重新评审。

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
- 栈/内存：doc22/23（stackman/vstack）。
