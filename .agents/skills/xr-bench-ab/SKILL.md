---
name: xr-bench-ab
description: >
  跑 xruntime 基准与性能测量取证的正确姿势。适用于:跑/解读 B1/B2/B3 数字、
  做变体同窗 A/B 与回归判定、性能定靶(perf/strace/分段计时)、变体验收。
  触发词样例:跑基准、run-bench、B1/B2、同窗 A/B、性能回归、压测数字可疑、
  perf 定靶、eventfd/op、超噪声。
---

# xruntime 基准与性能测量纪律(吸收 PEL pel-bench-cross)

来源:`01-plan.md` §3 / `03-handoff-s4-s6.md` §2/§6 / `02-upper-bound-design.md`
§4–§5;方法论层在 `00-unified-constraints.md` §4.5,本 skill 是操作层。

## 1. 跑之前(口径对齐,错一项数字作废)

1. `scripts/env-check.sh`(核数/governor/遗留进程/绑核);本机
   **governor=powersave**(无 root 未切)→ 只信 ≥5% 或分布明确前移;
2. `pgrep -af 'bench_|xr_'` 清跨会话遗留进程(曾把环境问题伪装成回归);
3. `scripts/build.sh release`(sanitizer/构建与基准**不要并行**,抢核污染);
4. 绑核:生产者/消费者分核(`--wkcpu`/`--prodcpu`);
5. **同窗单变量 A/B**:`flags=0` 与变体必须**同一 binary、同 session** 重测;
   跨 session 的 S2/S3 基线只作参考(handoff §6.4);
6. 日志自动落 `bench-logs/`;失败先取 `-latest.log`,不重跑拿日志。

## 2. 命令

```bash
scripts/run-bench.sh release bench_env --iters 200000 [--cpu N]

# B1 唤醒延迟分解(段:producer_side/consumer_wake/rtt)
scripts/run-bench.sh release bench_roundtrip --ops 200000 --wkcpu 1 --prodcpu 2
scripts/run-bench.sh release bench_roundtrip --ops 200000 --same-thread [--flags N]

# B2 每请求唤醒税(fan-in M→1;--wait=请求-响应口径,复刻 PEL D5)
scripts/run-bench.sh release bench_fanin --producers 8 --ops 200000 --wkcpu 1 --prodcpu 2 --wait [--flags N]
```

上限路径基准(B4 parker 微基准 / B5 transport 批曲线 / B6 同机 tokio 参照)
设计见 `02-upper-bound-design.md` §5。

## 3. 数字解读

- **3 轮 median**;轮方差 >5% 的轮作废重跑;
- B1:跨线程 TSC 负差(t2<t1)属正常,聚合钳 0 并记 `neg_delta`;比较看
  p50/avg,别被 max 带偏;
- 已沉淀基线(release,flags=0):B1 cross RTT **3.0–3.1µs**(producer_side
  ~1.29µs / consumer_wake ~1.78µs);B1 same-thread RTT **50–70ns**;
  B2 wait M=1 eventfd/op **~0.99**、M=8 **~0.124**;B2 unpaced M=1 0.002;
- **同线程 `wake_writes≈2` 是 pending 合并的效果,不是 V1 的收益**(handoff §6.3);
- **单 subject 单面异常 = 代码;多 subject 同步同幅异常 = 环境**;
- `--wait` 是请求-响应口径;`unpaced` 是合并上限形态——两者不可混比。

## 4. 定靶与取证(证据链先于修法)

1. **单变量 A/B**:只允许一个差异;先测"理想形态"变体拿收益上界,再评估实现
   能吃到几成;
2. **收口 stats 优先**(`wake_writes/delivers/wake_direct/wake_gated`),其次
   `strace -c -f -e trace=write,epoll_wait,futex -p <pid>`(`timeout -s INT` 限时,
   防挂 teardown);
3. **perf 先于读码猜想**:`perf record -F 999 -g -p <pid> -- sleep 6` +
   `perf report --no-children --percent-limit 1.2`;
4. 收益假设有**实测否决权**:预估再合理,实测否决就回退/重定性;
5. **正负都落账**:负结果同样写台账并给选型结论,不许只报喜。

## 5. 变体验收流程(handoff §3.5,每变体照走)

1. 实现 flag(默认 `flags=0` = 纯 V0 行为不变)→ 2. 扩 `test_worker` 覆盖该
flag → 3. **debug 构建 + 相关测试绿**(按需 asan/tsan;五面全量按里程碑,
2026-10-02 分级)→ 4. 同窗 A/B:flags=0 与变体各 3 轮
median,B1 same/cross + B2 wait/unpaced 至少各一组 → 5. **超噪声(≥5% 或分布
明确前移)才保留默认**;否则保留 flag 并标"未超噪声",台账留档 → 6. 更新
`docs/01-plan.md` 台账 + README 状态表。

## 6. 已知陷阱速查

| 陷阱 | 后果 |
|---|---|
| 未 3 轮 median / 跨 session 比基线 | 数字不可比 |
| 遗留 bench 进程 | 假回归/假劣化 |
| governor=powersave + 跨 session | 漂移被当收益 |
| 同线程停止条件用"两 task hops 求和" | 误判(S3 教训,handoff §6.1) |
| V2 门控缺"先置 sleeping 再复核" | 丢唤醒,tsan 未必报(handoff §1.2) |
| `flags` 未在 `xr_task_spawn` 前设置 | 变体不生效(handoff §6.6) |
| 手测不带 taskset | 绝对值不可比 |
| 基准期间并行门禁/构建 | 抢核污染 |
