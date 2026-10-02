# xruntime 上下文与协作规则(吸收 PEL `.agents/skills/SKILL.md`,结合本仓实际)

## 文档地图(先读哪份)

| 文档 | 角色 | 何时读 |
|---|---|---|
| `docs/00-unified-constraints.md` | **约束唯一正文**(吸收 PEL 00:统一收口/G1–G5/L0–L6/P1–P16) | 改动前必读 §0–§4 |
| `docs/01-plan.md` | 计划/假设 H1–H6/变体 V0–V5/基准 B1–B3/台账 | 认领任务先读 |
| `docs/02-upper-bound-design.md` | 上限路径设计(参照 Tokio/Loom/HotSpot,V6–V9、L0 选型) | 做上限变体时 |
| `docs/03-handoff-s4-s6.md` | S4–S6 实施交接(接口/契约/坑/检查表) | 接手 S4+ 时先读 §1–§2 |

## 工作规则

- 长任务先落计划文档(整体+每步),完成子任务即更新台账,结束总结;
- 除非必要不整文读大文件;检索用 `rg`/glob,读文件用 Read 分段;
- 尽量不用全局变量和静态变量(现有 TLS `tls_worker` 为已登记例外);
- 运行复杂查询后清理无关中间日志;构建/测试/基准日志一律落盘
  (`test-logs/`、`bench-logs/`,已 gitignore),失败先取 `-latest.log`,**不为拿日志重跑**;
- 约束能落地就进 CI/结构守卫,不靠文档自觉(`00-unified-constraints.md` §0/§5);
- 语言:C11,`-Wall -Wextra -Werror`,**无第三方**(纯 Linux 原语:epoll/eventfd/
  futex;io_uring 仅裸 syscall 且可选);
- 构建在 `build-<profile>[-clang]/`;测试 `scripts/run-tests.sh <profile>`;
  基准 `scripts/run-bench.sh <profile> <bench> [args]`;跑基准前 `scripts/env-check.sh`;
- **提交纪律**:提交身份 `qihaoxi <qihao.xi@foxmail.com>`(仓库既有作者),
  消息风格 `feature: S<N> <中文摘要>`(正文列改动+数字+判定);**未获指示不 push**;
- 另一个会话可能在并行改本仓:动 `src/`/`include/xr/` 前先 `git status`/`git log`,
  避免覆盖;只改自己任务范围内的文件。

## 可用 skill

| skill | 用途 |
|---|---|
| `xr-sanitizer` | sanitizer 面分级(默认 debug;按需 asan/tsan/ubsan;五面全量按里程碑)与日志/坑 |
| `xr-bench-ab` | 基准口径、同窗 A/B、定靶取证、变体验收流程 |
| `xr-perf-tools` | 测量工具选层(TSC/perf/strace/bpftrace/runqlat)与观测坑 |
| `xr-fix-regression-audit` | 并发/生命周期/调度时机/数据结构替换类改动的回归审计 |
