#!/usr/bin/env bash
# 测量前环境体检:核数/governor/负载/遗留进程/当前 affinity。
# 判据(借鉴 PEL 纪律):governor 非 performance 或存在遗留 xr 进程时,
# 测量结论一律存疑。
set -uo pipefail

echo "== cpu =="
nproc

echo "== governor =="
govs="$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null)"
if [ -n "$govs" ]; then
	printf '%s\n' "$govs" | sort | uniq -c
else
	echo "(no cpufreq sysfs)"
fi

echo "== loadavg =="
cat /proc/loadavg

echo "== leftover xr/bench procs =="
pgrep -af 'bench_|test_parker|test_smoke' || echo none

echo "== affinity of shell =="
taskset -pc $$ 2>/dev/null || echo "(taskset unavailable)"
