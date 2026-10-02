---
name: xr-perf-tools
description: >
  选择/使用性能测量工具:TSC 进程内插桩、perf、strace、bpftrace/eBPF、
  runqlat/perf c2c 等;判断"数字怎么量出来的、该用哪层工具、有哪些坑"。
  适用于:性能定靶、数字可疑、选测量手段、学习 perf/BPF、写基准插桩。
  触发词样例:怎么测量、perf、bpftrace、eBPF、strace、TSC、火焰图、
  cache miss、伪共享、runqlat、观测开销、测量方法。
---

# xruntime 性能测量工具箱(操作层)

方法学正文:`docs/05-measurement-toolbox.md`(学习向);
纪律权威:`docs/00-unified-constraints.md` §4.5;口径/验收:`xr-bench-ab` skill。

## 选层(能用下层不用上层)

| 问题 | 手段 |
|---|---|
| 某段 ns/每请求次数/合并率 | **L0 TSC + 收口点计数**(本仓已有,零外依赖) |
| 哪个函数吃 CPU / IPC / cache miss | L1 `perf record/report`、`perf stat` |
| 每请求 syscall 次数 | L2 `strace -c`(只信次数,**ptrace 会压慢进程**)或 L3 tracepoint |
| 内核/用户函数级计数与直方图 | L3 `bpftrace` kprobe/uprobe/tracepoint |
| "慢"还是"没被调度" | L4 `runqlat`、`offcputime`、`perf sched` |
| 多核 cache line 争用 | L4 `perf c2c`(HITM) |

## 本仓 L0 速查

```c
xr_time_init(); uint64_t t0 = xr_tsc(); ... xr_tsc_to_ns(xr_tsc()-t0);
/* 计数只在收口点:worker_wake / worker_futex_wake / worker_run_task ... */
```
- 逐 op 存样本 → p50/p90/p99;跨线程 TSC 负差钳 0 并记 neg_delta;
- 计数器别放热字段同 cache line(饱和形态逐 deliver 计数 ≈10% 税);
- 先量工具税(bench_env:now_ns=19ns、tsc=11ns),几十 ns 的段要写进报告。

## perf / strace / bpftrace 常用式

```bash
perf stat -e cycles,instructions,branches,cache-misses,context-switches -p <pid> -- sleep 6
perf record -F 999 -g -p <pid> -- sleep 6 && perf report --no-children --percent-limit 1.2
perf c2c record -F 999 --all-user -p <pid> -- sleep 6 && perf c2c report
timeout -s INT 8 strace -c -f -e trace=write,epoll_wait,futex -p <pid>
bpftrace -e 'kprobe:eventfd_write /pid == 1234/ { @writes = count(); }'
bpftrace -e 'tracepoint:syscalls:sys_enter_futex /pid == 1234/ { @[args->op & 0x7f] = count(); }'
runqlat 5 1; offcputime -p <pid> 5
```

## 坑(先读)

- **strace 串行化**:高并发下 170× 级失真(PEL doc148 §0.2),只信比值;
- **uretprobe 禁区**:跨 park/switch 函数返回序反转,禁 return probe;
- **权限**:bpftrace/perf 常需 root 或 CAP_BPF/CAP_PERFMON,容器内多不可用;
- **governor/绑核/遗留进程**:先 `scripts/env-check.sh`,powersave 只信同窗相对值;
- **单变量同窗**:A/B 同 binary 同 session 交错;≥5% 或分布前移才保留;
- **负向对照**:守卫/探针必须注入违规样本验证能咬人;
- perf 先于读码猜想;环境异常先查环境再归因代码(doc146 §0.5)。
