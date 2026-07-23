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

test_configured_server_falls_back_to_localhost
test_noninteractive_error_lists_all_attempts
test_configured_server_stops_discovery_on_success
printf 'CodeQL 探测脚本回归测试通过\n'
