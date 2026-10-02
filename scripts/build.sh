#!/usr/bin/env bash
# 用法: scripts/build.sh [debug|release|asan|tsan|ubsan] [额外 CMake 参数...]
# 日志落盘 test-logs/build-<profile>-<ts>.log(附 -latest.log 软链)。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
profile="${1:-debug}"
shift || true

case "$profile" in
	debug)   build_type=Debug;          sanitize=none ;;
	release) build_type=Release;        sanitize=none ;;
	asan)    build_type=RelWithDebInfo; sanitize=address ;;
	tsan)    build_type=RelWithDebInfo; sanitize=thread ;;
	ubsan)   build_type=RelWithDebInfo; sanitize=undefined ;;
	*)
		echo "unknown profile: $profile (debug|release|asan|tsan|ubsan)" >&2
		exit 2
		;;
esac

# XR_CC=clang ./scripts/build.sh debug → build-debug-clang(同 profile 可并存多编译器)
compiler="${XR_CC:-}"
name="$profile"
cmake_cc_args=()
if [ -n "$compiler" ]; then
	name="$profile-$compiler"
	cmake_cc_args=(-DCMAKE_C_COMPILER="$compiler")
fi

build_dir="$root/build-$name"
log_dir="$root/test-logs"
mkdir -p "$log_dir"
ts="$(date +%Y%m%d-%H%M%S)"
log="$log_dir/build-$name-$ts.log"
ln -sf "$(basename "$log")" "$log_dir/build-$name-latest.log"

{
	echo "== build profile=$profile sanitize=$sanitize cc=${compiler:-${CC:-cc}} =="
	cmake -S "$root" -B "$build_dir" -G Ninja \
		-DCMAKE_BUILD_TYPE="$build_type" \
		-DXR_SANITIZE="$sanitize" "${cmake_cc_args[@]}" "$@"
	cmake --build "$build_dir"
} 2>&1 | tee "$log"

echo "log: $log"
