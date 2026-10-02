#!/usr/bin/env bash
# 测量前环境体检:核数/governor/负载/遗留进程/当前 affinity。
# 判据(借鉴 PEL 纪律):governor 非 performance 或存在遗留 xr 进程时,
# 测量结论一律存疑。
set -uo pipefail

echo "== cpu =="
nproc

echo "== governor =="
found=0
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
	if [ -f "$g" ]; then
		cat "$g"
		found=1
	fi
done | sort | uniq -c || true
[ "$found" -eq 1 ] || echo "(no cpufreq sysfs)"

echo "== loadavg =="
cat /proc/loadavg

echo "== leftover xr/bench procs =="
pgrep -af 'bench_|test_parker|test_smoke' || echo none

echo "== affinity of shell =="
taskset -pc $$ 2>/dev/null || echo "(taskset unavailable)"
