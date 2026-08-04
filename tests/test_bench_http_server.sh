#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP_DIR="$(mktemp -d)"
BENCHMARK_BINARY="$PROJECT_ROOT/bin/bench_http_server"
FAIL_SOCKET_LIBRARY="$TEST_TMP_DIR/fail_socket.so"

cleanup() {
  rm -rf "$TEST_TMP_DIR"
}
trap cleanup EXIT

fail() {
  printf '失败: %s\n' "$*" >&2
  exit 1
}

[ -x "$BENCHMARK_BINARY" ] || fail "未构建 HTTP 基准程序: $BENCHMARK_BINARY"

"${CC:-cc}" -shared -fPIC "$PROJECT_ROOT/tests/fixtures/fail_socket.c" -o "$FAIL_SOCKET_LIBRARY"

set +e
output="$(timeout --kill-after=1s 2s env LD_PRELOAD="$FAIL_SOCKET_LIBRARY" "$BENCHMARK_BINARY" \
  --benchmark_filter=HttpGet --benchmark_min_time=0.001s 2>&1)"
status=$?
set -e

case "$status" in
  0) fail "HTTP 基准服务初始化失败后不应报告成功: $output" ;;
  124|137) fail "HTTP 基准服务初始化失败后不应自旋至超时: $output" ;;
esac

case "$output" in
  *"HTTP 基准服务初始化失败"*) ;;
  *) fail "缺少明确的初始化失败错误: $output" ;;
esac

set +e
output="$(timeout --kill-after=1s 20s "$BENCHMARK_BINARY" \
  --benchmark_filter='^HttpServerBench/(HttpGet|HttpPost)$' --benchmark_min_time=0.001s 2>&1)"
status=$?
set -e

[ "$status" -eq 0 ] || fail "HTTP 基准正常运行应成功: $output"

case "$output" in
  *"HttpServerBench/HttpGet"*"HttpServerBench/HttpPost"*) ;;
  *) fail "HTTP 基准正常运行未覆盖连续的 fixture 生命周期: $output" ;;
esac

printf 'HTTP 基准初始化失败快速退出回归通过\n'
