#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP_DIR="$(mktemp -d)"
FAKE_BIN_DIR="$TEST_TMP_DIR/bin"

cleanup() {
  rm -rf "$TEST_TMP_DIR"
}
trap cleanup EXIT

fail() {
  printf '失败: %s\n' "$*" >&2
  exit 1
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local description="$3"

  if [ "$expected" != "$actual" ]; then
    fail "$description，期望: [$expected]，实际: [$actual]"
  fi
}

assert_contains() {
  local text="$1"
  local expected="$2"
  local description="$3"

  if [[ "$text" != *"$expected"* ]]; then
    fail "$description，未找到: [$expected]，输出: [$text]"
  fi
}

mkdir -p "$FAKE_BIN_DIR"
cat > "$FAKE_BIN_DIR/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

url="${!#}"
printf '%s\n' "$url" >> "$FAKE_CURL_LOG"

if [ "$url" = "${FAKE_CURL_SUCCESS_URL%/}/" ]; then
  printf '204'
  exit 0
fi

exit 7
EOF
chmod +x "$FAKE_BIN_DIR/curl"

# 仅加载待测函数，避免执行实际 CodeQL 提交流程。
source "$PROJECT_ROOT/scripts/codeql.sh"

test_configured_server_falls_back_to_localhost() (
  export PATH="$FAKE_BIN_DIR:$PATH"
  export FAKE_CURL_LOG="$TEST_TMP_DIR/fallback.log"
  export FAKE_CURL_SUCCESS_URL="http://localhost:8080"
  export CODEQL_SERVER_URL="http://configured.invalid:8080"
  : > "$FAKE_CURL_LOG"

  discover_server

  assert_equals "http://localhost:8080" "$CODEQL_SERVER_URL" "localhost 应成为最终服务器"
  assert_equals $'http://configured.invalid:8080/\nhttp://localhost:8080/' "$(cat "$FAKE_CURL_LOG")" \
    "配置地址失败后应按顺序探测 localhost"
)

test_noninteractive_error_lists_all_attempts() (
  export PATH="$FAKE_BIN_DIR:$PATH"
  export FAKE_CURL_LOG="$TEST_TMP_DIR/noninteractive.log"
  export FAKE_CURL_SUCCESS_URL="http://never-success.invalid:8080"
  export CODEQL_SERVER_URL="http://configured.invalid:8080"
  : > "$FAKE_CURL_LOG"

  local output
  local status
  set +e
  output="$(discover_server </dev/null 2>&1)"
  status=$?
  set -e

  assert_equals "1" "$status" "全部探测失败时应返回失败"
  assert_contains "$output" "已尝试地址:" "非交互错误应说明已尝试地址"
  assert_contains "$output" "http://configured.invalid:8080" "错误应列出配置地址"
  assert_contains "$output" "http://localhost:8080" "错误应列出 localhost"
  assert_equals $'http://configured.invalid:8080/\nhttp://localhost:8080/' "$(cat "$FAKE_CURL_LOG")" \
    "非交互失败也应先后探测两个地址"
)

test_configured_server_stops_discovery_on_success() (
  export PATH="$FAKE_BIN_DIR:$PATH"
  export FAKE_CURL_LOG="$TEST_TMP_DIR/configured-success.log"
  export FAKE_CURL_SUCCESS_URL="http://configured.valid:8080"
  export CODEQL_SERVER_URL="http://configured.valid:8080/"
  : > "$FAKE_CURL_LOG"

  discover_server

  assert_equals "http://configured.valid:8080" "$CODEQL_SERVER_URL" "成功配置地址应保留并去除末尾斜杠"
  assert_equals "http://configured.valid:8080/" "$(cat "$FAKE_CURL_LOG")" \
    "配置地址成功时不应探测 localhost"
)

test_package_source_excludes_report_artifacts() (
  local archive="$TEST_TMP_DIR/source.tar.gz"
  local contents

  package_source "$archive"
  contents="$(tar tzf "$archive")"

  if [[ "$contents" == *"benchmark/reports/"* ]] || [[ "$contents" == *"benchmark/report/"* ]]; then
    fail 'CodeQL 源码包不得包含新旧 benchmark 报告产物'
  fi
  assert_contains "$contents" 'benchmark/bench_' 'CodeQL 源码包必须保留 benchmark C++ 源码'
  assert_contains "$contents" 'benchmark/tools/migrate_legacy_reports.py' 'CodeQL 源码包必须保留迁移工具'
  assert_contains "$contents" 'xmake.lua' 'CodeQL 源码包必须保留 xmake.lua'
)

test_codeql_long_running_timeout_configuration() (
  local value output

  unset CODEQL_SUBMIT_TIMEOUT CODEQL_POLL_TIMEOUT CODEQL_POLL_INTERVAL CODEQL_POLL_ATTEMPTS
  value="$(codeql_timeout_value CODEQL_SUBMIT_TIMEOUT 1800)"
  assert_equals 1800 "$value" 'CodeQL 提交超时默认应为 1800 秒'
  value="$(codeql_timeout_value CODEQL_POLL_TIMEOUT 1800)"
  assert_equals 1800 "$value" 'CodeQL 轮询总预算默认应为 1800 秒'

  for value in 600 7200; do
    CODEQL_SUBMIT_TIMEOUT="$value"
    assert_equals "$value" "$(codeql_timeout_value CODEQL_SUBMIT_TIMEOUT 1800)" \
      "CODEQL_SUBMIT_TIMEOUT=$value 必须被接受"
  done
  for value in 599 7201 invalid; do
    if output="$(CODEQL_SUBMIT_TIMEOUT="$value" codeql_timeout_value CODEQL_SUBMIT_TIMEOUT 1800 2>&1)"; then
      fail "CODEQL_SUBMIT_TIMEOUT=$value 必须被拒绝"
    fi
    assert_contains "$output" 'CODEQL_SUBMIT_TIMEOUT' '超时配置错误必须说明变量名'
    assert_contains "$output" "$value" '超时配置错误必须说明实际值'
    assert_contains "$output" '600..7200' '超时配置错误必须说明合法范围'
  done

  unset CODEQL_POLL_ATTEMPTS
  CODEQL_POLL_TIMEOUT=1800 CODEQL_POLL_INTERVAL=5
  assert_equals 360 "$(codeql_poll_attempts)" '未设置 attempts 时必须按预算计算轮询次数'
  CODEQL_POLL_ATTEMPTS=7
  assert_equals 7 "$(codeql_poll_attempts)" '显式 attempts 必须作为轮询上限保留兼容语义'
  for value in 0 -1 invalid; do
    if output="$(CODEQL_POLL_INTERVAL="$value" codeql_poll_attempts 2>&1)"; then
      fail "CODEQL_POLL_INTERVAL=$value 必须在网络请求前被拒绝"
    fi
    assert_contains "$output" 'CODEQL_POLL_INTERVAL' '间隔配置错误必须说明变量名'
    assert_contains "$output" "$value" '间隔配置错误必须说明实际值'
  done
)

test_poll_result_sends_first_request_with_exact_budget() (
  local response_file="$TEST_TMP_DIR/poll-response.json"
  local output status

  export PATH="$FAKE_BIN_DIR:$PATH"
  export FAKE_CURL_LOG="$TEST_TMP_DIR/poll-events.log"
  export FAKE_CURL_SUCCESS_URL="http://never-success.invalid:8080"
  export CODEQL_SERVER_URL="http://configured.valid:8080"
  export CODEQL_POLL_TIMEOUT=600
  export CODEQL_POLL_INTERVAL=600
  unset CODEQL_POLL_ATTEMPTS
  : > "$FAKE_CURL_LOG"
  SECONDS=0
  sleep() {
    printf 'sleep:%s\n' "$1" >> "$FAKE_CURL_LOG"
    SECONDS=$((SECONDS + 10#$1))
  }

  set +e
  output="$(poll_result task-600 "$response_file" 2>&1)"
  status=$?
  set -e

  assert_equals "1" "$status" "未就绪结果应在单次轮询后失败"
  assert_equals "http://configured.valid:8080/result/task-600" "$(cat "$FAKE_CURL_LOG")" \
    "整除预算的首次 attempt 必须立即发送且仅发送一次 /result 请求"
  assert_contains "$output" "最多 1 次" "整除预算应调度一次轮询"
)

test_configured_server_falls_back_to_localhost
test_noninteractive_error_lists_all_attempts
test_configured_server_stops_discovery_on_success
test_package_source_excludes_report_artifacts
test_codeql_long_running_timeout_configuration
test_poll_result_sends_first_request_with_exact_budget
printf 'CodeQL 探测脚本回归测试通过\n'
