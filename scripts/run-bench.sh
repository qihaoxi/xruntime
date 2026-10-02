#!/usr/bin/env bash
# 用法: scripts/run-bench.sh <profile> <binary> [args...]
#   例: scripts/run-bench.sh release bench_env --cpu 2
# 日志落盘 bench-logs/<binary>-<ts>.log。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
profile="${1:?usage: run-bench.sh <profile> <binary> [args...]}"
shift
binary="${1:?usage: run-bench.sh <profile> <binary> [args...]}"
shift

bin_path="$root/build-$profile/bench/$binary"
if [ ! -x "$bin_path" ]; then
	echo "bench binary missing: $bin_path (run scripts/build.sh $profile)" >&2
	exit 2
fi

log_dir="$root/bench-logs"
mkdir -p "$log_dir"
log="$log_dir/$binary-$(date +%Y%m%d-%H%M%S).log"

"$bin_path" "$@" 2>&1 | tee "$log"
echo "log: $log"
