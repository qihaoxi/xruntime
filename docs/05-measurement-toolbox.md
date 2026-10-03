# xruntime 05 — 测量工具箱:从进程内插桩到 eBPF

日期:2026-10-03 · 状态:生效 · 性质:**方法+工具速查**(学习向)
上游:`docs/00-unified-constraints.md` §4.5(测量纪律)、`docs/01-plan.md` §3、
skill `xr-bench-ab`(操作层);PEL 方法学权威:doc146 §0.5、doc151 §4.3。

> 核心纪律:**先问"这个数字是怎么量出来的",再问"它说明什么"。**
> 工具只提供观测面;结论必须过同窗单变量 A/B + 环境体检 + 证据链。

## 1. 层级总览

| 层 | 手段 | 能回答 | 代价/盲区 |
|---|---|---|---|
| **L0 进程内插桩** | TSC 分段、收口点原子计数 | 段级 ns、transport/req、合并率 | 改代码;观测本身有开销;跨核 TSC 偏斜 |
| **L1 perf 计数器** | `perf stat/record/report` | 热点函数、IPC、cache miss、上下文切换 | 采样扰动小;符号需 debug 信息 |
| **L2 syscall 追踪** | `strace -c/-f`、ltrace | 每请求 syscall 次数/种类 | **ptrace 串行化会重压被测进程**(PEL 实测 170×) |
| **L3 eBPF/bpftrace** | kprobe/uprobe/tracepoint | 内核+用户函数级计数/直方图,低扰动 | 需权限;uretprobe 跨 fibre 切换危险 |
| **L4 调度/缓存专项** | runqlat/offcputime、`perf sched/c2c` | 排队延迟、谁阻塞、伪共享 | 内核版本/权限;解释需经验 |

原则:**能用 L0 回答的不要上 L1–L4**(L0 最精确、零外部依赖);L0 量不出
的"为什么慢"用 L1/L3 定靶,再用 L0 做同窗验证。

## 2. L0 进程内插桩(本仓主力)

### 2.1 TSC 分段模板

```c
xr_time_init();                  /* 20ms 窗口校准 tsc_hz(~3.79GHz) */
uint64_t t0 = xr_tsc();          /* rdtsc ≈11ns/次 */
...被测段...
uint64_t ns = xr_tsc_to_ns(xr_tsc() - t0);
```

- 逐 op 存样本数组 → 排序取 p50/p90/p99(**不要只看 avg**:B1 的 max 常被
  调度抖动带到 ms 级);
- 段必须落在**统一收口点**(constraints §0):如 B1 的 t0=unpark 进入、
  t1=unpark 返回、t2=step 开始;
- 跨线程段:两线程各记 TSC,比较时接受负差(t2<t1 正常,缓存/乱序),聚合
  钳 0 并记 `neg_delta` 数(B1 已有)。

### 2.2 收口点计数(比时间更可靠)

```c
atomic_fetch_add_explicit(&w->stat_wake_writes, 1, memory_order_relaxed);
```

- 只允许在收口函数内更新(constraints §0 守卫:散点即红);
- 计数类指标(transport/req、合并率、parks/deliver)不受 TSC 噪声影响,
  **判变体收益时优先看它们**;
- 坑:计数器与热字段同 cache line 时,多生产者 fetch_add 会引入伪共享
  (S5 实测:饱和形态下逐 deliver 计数是 ~10% 级税,已从 FUTEX 热路径移除)。

### 2.3 观测自身的成本

- 先量工具的税:`bench_env` 实测 `now_ns=19ns/op`、`tsc_read=11ns/op`;
  若被测段本身只有几十 ns,插桩占比必须写进报告;
- 对照法:插桩版 vs 非插桩版跑同一负载,差值为观测税上限。

### 2.4 L0 栈切换标定与限制(stackful 对照轮)

- 纯切换:`bench_l0 --mode calib`(两栈 `xr_cpu_switch` 往返,2N+2 次;
  本机 5.6ns/switch,PEL stackman ~15ns 为对照上界);
- 任务级:`bench_roundtrip --l0=stackless|fibre`(机制/测量点不变,只换
  body 与 resume 原语;t2 语义 fibre 在 wait 恢复后,含消费 CAS);
- 创建/销毁:`bench_l0 --mode create`(100k,16KB 栈)+ `/proc/self/status`
  VmPeak/VmRSS;本机 141ns vs 7.20µs/个;
- 限制:自定义栈与 ASan/TSan 不兼容(L0 测试仅 sanitizer=none 注册);
  mmap 栈非 PEL vstack(slot pool/madvise),VA/TLB 压力偏低 → stackful
  成本是**下限**,TLB/100K 驻留需 perf 补测。
- 驻留/TLB 补测(S8):
  - `bench_l0 --mode=ring --fibres N --ops <采样跳数>`:N 驻留节点 token-ring,
    主线程逐跳喂 token,报每跳 t0→t2 分布 + RSS(reside/end);
  - `sudo perf stat -e dtlb-loads,dtlb-load-misses,cycles,instructions -- ...`
    (本机 paranoid=3,需 sudo;perf 介入使绝对延迟 ~2×,只做相对比较);
  - **VMA 计数**:每 mmap+mprotect 产生 2 个 VMA,`vm.max_map_count` 默认
    65530 → fibre 并发上限 ~32.7K;测大规模驻留前需临时放宽并恢复;
  - 读 RSS 用 `/proc/self/status` 的 VmPeak/VmRSS(mode=create/ring 已内建);
  - park 次数曲线:`bench_echo --ack=park --batch B`(driver 在途窗口 B,
    每 B 个完成 park 一次;B=1 旧口径);看 `driver-parks/req`、`futex/req`
    与 rps/p99 的折中(B 增大 p99 批尾上升)。

## 3. L1 perf(热点与硬件计数)

```bash
# 计数:IPC/分支/缓存/切换(先看这些再读码)
perf stat -e cycles,instructions,branches,branch-misses,cache-misses,\
context-switches,cpu-migrations -p <pid> -- sleep 6

# 采样火焰:热点函数(先 perf 后读码猜想,PEL doc146 §0.5)
perf record -F 999 -g -p <pid> -- sleep 6
perf report --no-children --percent-limit 1.2

# 调度:谁让出/排队多久
perf sched record -- sleep 3 && perf sched latency

# 伪共享(多核 cache line 争用,HITM)
perf c2c record -F 999 --all-user -p <pid> -- sleep 6 && perf c2c report

# 锁争用
perf lock record -p <pid> -- sleep 3 && perf lock report
```

- 编译需 `-g`(本仓 debug/release 均带);
- `perf report` 符号里若只见 `[unknown]`,检查 debug 信息与 `kptr_restrict`;
- **perf 是定靶工具**:找到 50% CPU 的符号后再改码,别用理论推演代替
  (PEL D4 v2 两轮误归因的教训)。

## 4. L2 syscall 追踪(计数,不追时序)

```bash
# 每请求 syscall 次数(限时,防挂 teardown)
timeout -s INT 8 strace -c -f -e trace=write,writev,epoll_wait,epoll_ctl,futex \
  -p <pid>

# 看单次调用参数(如 futex op/uaddr)
timeout -s INT 5 strace -f -e trace=futex -p <pid>
```

- **陷阱**:ptrace 会串行化线程,高并发下把被测进程压慢一个量级
  (PEL doc148 §0.2:libuv-raw 133K→780 rps)。只信**比值/每请求次数**,
  不信绝对吞吐;
- 对照 eBPF(§5)交叉验证 syscall 计数;
- 本机若未装 strace,用 eBPF tracepoint 替代。

## 5. L3 eBPF / bpftrace(低扰动,函数级)

```bash
# 计数:eventfd 写次数/请求(PEL doc151 §4.3 的方法)
bpftrace -e 'kprobe:eventfd_write /pid == 1234/ { @writes = count(); }'

# futex 调用分布(op 低 7 位:WAIT/WAKE/...)
bpftrace -e 'tracepoint:syscalls:sys_enter_futex /pid == 1234/ \
  { @[args->op & 0x7f] = count(); }'

# 延迟直方图(函数进入→返回)
bpftrace -e 'kprobe:worker_futex_wake { @s[tid] = nsecs; }
             kretprobe:worker_futex_wake /@s[tid]/ \
             { @ns = hist(nsecs - @s[tid]); delete(@s[tid]); }'

# 用户态函数:uprobe(注意符号与 -g)
bpftrace -e 'uprobe:/path/bin:worker_futex_wake { @ = count(); }'

# 现成 bcc 工具
runqlat          # 运行队列排队延迟
offcputime       # 谁被阻塞了多久(按栈)
funclatency      # 函数延迟直方图
```

- **权限**:需要 root/CAP_BPF+CAP_PERFMON;容器内常不可用;
- **uretprobe/return probe 禁区**:跨 park/switch 的函数上返回序会反转
  (fibre 切换后返回落在另一条栈),PEL doc08 §24.2 规定此类函数禁 uretprobe,
  uprobe 计数安全;
- `-p PID` / `/pid == N/` 先窄化,避免全系统放大;
- bpftrace 单行表达式用 `-e`,脚本模式适合多探针。

## 6. L4 调度与缓存专项

```bash
# 绑核(基准必做):生产者/消费者分核
taskset -c 1 ./worker &          # 或代码内 sched_setaffinity
taskset -c 2 ./producer

# 运行队列延迟(判断"慢"还是"没被调度")
runqlat 5 1

# 环境体检(本仓脚本)
scripts/env-check.sh             # 核数/governor/遗留进程/affinity
```

- `governor=performance` 才信绝对值;powersave 下只信同窗相对值;
- `isolcpus`/`nohz_full`/`THP`(always 会影响 vstack/RSS 类测量)要在报告里
  声明;
- 多核 TSC 可能不同步:跨核时间戳只用于粗粒度;段内计时绑核后更稳。

## 7. 方法学(工具之上)

1. **环境先于归因**:遗留进程/抢核/温度可造"像回归"的数据;多 subject 同步
   同幅异常=环境,单 subject 单面异常=代码;
2. **同窗单变量**:A/B 只允许一个差异,同一 binary、同一 session,交错
   (ABAB/BAAB)消除漂移;跨 session 基线只作参考;
3. **warmup + ≥3s 测量 + 3 轮 median**;轮方差 >5% 作废重跑;
4. **判据**:≥5% 或延迟分布明确前移才保留,否则回退留档;
5. **负向对照**:新守卫/新门禁必须注入违规样本验证能咬人(否则是摆设);
6. **证据链先于修法**:现场/core → 读码 → sanitizer 首错;候选修法先测后定,
   被否留档(PEL doc101);
7. **正负都落账**:负结果同样写台账并给选型结论。

## 8. xruntime 命令速查

```bash
scripts/env-check.sh
scripts/build.sh release
scripts/run-bench.sh release bench_env --iters 200000 --cpu 1
scripts/run-bench.sh release bench_roundtrip --ops 200000 --wkcpu 1 --prodcpu 2 --flags 32
scripts/run-bench.sh release bench_fanin --producers 8 --ops 200000 --wkcpu 1 --prodcpu 2 --wait --flags 0
scripts/run-bench.sh release bench_echo --conns 256 --drivers 4 --ops 20000 --wkcpu 1 --drcpu 2 --ack=park --flags 32
# U12 多 loop accept 分发(内嵌 PEL loadgen;echo 1KB)
LG=~/workspace/raw-spofer-pel/polyglot-c/benchmarks/cross/work/loadgen
scripts/run-bench.sh release bench_scale --workers 4 --dist reuseport --port 23100 \
    --duration 5 --conns 256 --loadgen $LG
scripts/run-bench.sh release bench_scale --workers 4 --dist dispatch --port 23100 \
    --duration 5 --conns 256 --loadgen $LG
# 锁-free 语义 per-core 收益(U13a):local 无锁 vs atomic/mutex 共享
scripts/run-bench.sh release bench_scale --workers 8 --dist reuseport --port 23100 \
    --duration 5 --conns 256 --work local --work-iters 1024 --loadgen $LG
scripts/run-bench.sh release bench_scale --workers 8 --dist reuseport --port 23100 \
    --duration 5 --conns 256 --work atomic --work-iters 1024 --loadgen $LG
# L0 迁移边界与标定
scripts/run-bench.sh release bench_l0 --mode migrate --ops 200000
scripts/run-bench.sh release bench_l0 --mode ring --l0 fibre --fibres 100000 --ops 2000000 --stack 16384
# D8 self-wake 代理:同线程 eventfd 写→epoll 返回→drain
scripts/run-bench.sh release bench_l0 --mode wake --ops 2000000
# U13c 饱和点:bench_scale 内建 getrusage(server CPU/req、nvcsw)
scripts/run-bench.sh release bench_scale --workers 8 --dist reuseport --port 23100 \
    --duration 5 --conns 512 --threads 8 --payload 256 --loadgen $LG
# 日志: bench-logs/ test-logs/(-latest.log 软链)
```

## 9. 参考

- PEL doc146 §0.5(方法学权威:环境先于代码、perf 先于猜想、cache 常数)、
  doc148 §0.2(strace 串行化实锤)、doc151 §4.3(kprobe eventfd 计数)、
  doc08 §24.2(uretprobe×fibre 禁区);
- skill `xr-bench-ab`(口径/验收流程)、`xr-sanitizer`(门禁分级)、
  `xr-fix-regression-audit`(证据链/换出窗口);
- 外部:Linux `perf`(perf-stat/record/report/sched/c2c/lock)、
  `bpftrace`/bcc、`strace(1)`、`io_uring(7)`。
