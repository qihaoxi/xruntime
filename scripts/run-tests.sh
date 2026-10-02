#!/usr/bin/env bash
# 用法: scripts/run-tests.sh [debug|release|asan|tsan|ubsan]
# 日志落盘 test-logs/test-<profile>-<ts>.log(附 -latest.log 软链)。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
profile="${1:-debug}"
build_dir="$root/build-$profile"

if [ ! -d "$build_dir" ]; then
	echo "build dir missing: $build_dir (run scripts/build.sh $profile)" >&2
	exit 2
fi

log_dir="$root/test-logs"
mkdir -p "$log_dir"
ts="$(date +%Y%m%d-%H%M%S)"
log="$log_dir/test-$profile-$ts.log"
ln -sf "$(basename "$log")" "$log_dir/test-$profile-latest.log"

ctest --test-dir "$build_dir" --output-on-failure 2>&1 | tee "$log"
echo "log: $log"
