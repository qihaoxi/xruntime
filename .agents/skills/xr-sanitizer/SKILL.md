---
name: xr-sanitizer
description: >
  跑 xruntime 的 sanitizer 面(分级:默认 debug,按需 asan/tsan/ubsan)与
  构建/测试。适用于:定位内存错误/数据竞争/
  未定义行为;变体实现后的五面验证;用户问"怎么跑 sanitizer/测试/构建"。
  触发词样例:跑一下 sanitizer、asan/tsan/ubsan、五面绿、门禁、run-tests、
  build.sh、clang 构建。
---

# xruntime sanitizer 面(吸收 PEL pel-sanitizer-ci,结合本仓实际)

## 分级策略(2026-10-02 用户指示:全量门禁暂缓)

- **默认轻量(每次改动)**:`scripts/build.sh debug` + `scripts/run-tests.sh
  debug`(相关 ctest);快速、够抓功能回归;
- **按需单面**:内存/泄漏→`asan`;竞争/丢唤醒→`tsan`;UB/越界→`ubsan`;
  clang 双编译器纪律→`XR_CC=clang`(可选,碰编译器相关代码时);
- **五面全量**(gcc Debug、clang Debug、asan、tsan、ubsan 各 3/3):**按需/里程碑**
  ——阶段收口、提交前、用户要求;不再每变体强制。
- **恢复条件**:出现内存/竞争类缺陷、或进入发布/回灌评审时,恢复"核心目录
  改动五面全量"。

## 五面(需要时照此跑)

| face | 命令 | 抓什么 |
|---|---|---|
| gcc Debug | `scripts/build.sh debug` + `scripts/run-tests.sh debug` | 功能基线;gcc `-Wall -Wextra -Werror` |
| clang Debug | `XR_CC=clang scripts/build.sh debug` + `scripts/run-tests.sh debug` | 双编译器纪律(构建到 `build-debug-clang/`) |
| asan | `scripts/build.sh asan` + `scripts/run-tests.sh asan` | 内存错误 + 泄漏(默认 `detect_leaks=1`) |
| tsan | `scripts/build.sh tsan` + `scripts/run-tests.sh tsan` | 数据竞争(parker/registry/队列) |
| ubsan | `scripts/build.sh ubsan` + `scripts/run-tests.sh ubsan` | 未定义行为 |

- 构建目录 `build-<profile>[-clang]/`;release 用于基准(`scripts/build.sh release`);
- 日志自动落 `test-logs/build-<profile>-<ts>.log` / `test-<profile>-<ts>.log`(含
  `-latest.log` 软链);**失败先取已落盘日志,不为拿日志重跑**;
- 判 sanitizer 是否生效:看构建日志中的编译 flag,不要凭 profile 名猜。

## 门禁纪律(分级)

- 默认 `debug` 构建 + 相关 ctest 绿即可继续;**五面全量按需/里程碑**(见上),
  不再"核心目录改动即全量";
- 涉及 parker/worker 并发路径的改动,建议至少按需 `tsan` + `test_worker` 压测
  (丢唤醒 tsan 未必报,见坑);
- 改动前先对照 `docs/00-unified-constraints.md`(统一收口/生命周期/锁层次)
  与 `docs/01-plan.md`;变体实现按 `03-handoff-s4-s6.md` §3.5 流程;
- 可守卫化的约束做成结构守卫(见 constraints §0/§5),守卫随协议同生共死;
- **阴性对照**:新增守卫/断言必须注入违规样本验证能咬人,否则是摆设。

## 坑(吸收 PEL 实测,按本仓裁剪)

- **TSan 只证明"已执行路径无竞争"**:丢唤醒窗口(sleeping/pending 时序)tsan
  未必报——必须配 `test_worker` 与 B1/B2 压测(见 handoff §1.2/§1.3);
- **ASan 对自定义栈帧 UAF 零覆盖**(PEL doc90):本仓当前 stackless 无此面;
  L0 `stackman` 对照轮启用后,自定义栈帧出作用域不 poison——栈上 parker/ctx
  的 UAF 要另加语义断言 + TSan 碰撞拓扑;
- L0 stackman 对照轮启用时按 PEL 00 §9.2 做平台隔离(`-fno-sanitize=all` +
  ASan fiber API 的 SAVE/RESTORE),不要 ad-hoc 加宏;
- `governor=powersave`(本机无 root 未切):不影响正确性,但性能数字只信
  ≥5% 或分布明确前移(见 `xr-bench-ab`);
- 磁盘:五面构建目录并存;磁盘紧时逐面串行,每面跑完清理 `build-<profile>/`;
- 同线程 ping-pong 测试的停止条件勿用"两 task hops 求和"(handoff §6.1);
- 中间产物只落 `test-logs/`(gitignore),不污染工作区。
