#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP_DIR="$(mktemp -d)"
FIXTURE_ROOT="$TEST_TMP_DIR/project"
FAKE_BIN_DIR="$FIXTURE_ROOT/bin"

cleanup() {
  rm -rf "$TEST_TMP_DIR"
}
trap cleanup EXIT

fail() {
  printf '失败: %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local text="$1"
  local expected="$2"
  local description="$3"
  [[ "$text" == *"$expected"* ]] || fail "$description，未找到: [$expected]，输出: [$text]"
}

setup_fixture() {
  mkdir -p "$FIXTURE_ROOT/scripts/lib" "$FIXTURE_ROOT/src" "$FAKE_BIN_DIR"
  cp "$PROJECT_ROOT/scripts/lint.sh" "$FIXTURE_ROOT/scripts/lint.sh"
  cp "$PROJECT_ROOT/scripts/lib/common.sh" "$FIXTURE_ROOT/scripts/lib/common.sh"
  printf 'target("fixture")\n' > "$FIXTURE_ROOT/xmake.lua"
  printf 'int main() { return 0; }\n' > "$FIXTURE_ROOT/src/example.cpp"
  printf '[{"directory":".","file":"src/example.cpp","command":"c++ -c src/example.cpp"}]\n' \
    > "$FIXTURE_ROOT/compile_commands.json"

  cat > "$FAKE_BIN_DIR/xmake" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
  cat > "$FAKE_BIN_DIR/bc" <<'EOF'
#!/usr/bin/env bash
printf '0\n'
EOF
  cat > "$FAKE_BIN_DIR/clang-tidy" <<'EOF'
#!/usr/bin/env bash
printf 'clang-tidy %s\n' "$*" >> "$FAKE_TOOL_LOG"
exit "${FAKE_CLANG_TIDY_RC:-0}"
EOF
  cat > "$FAKE_BIN_DIR/cppcheck" <<'EOF'
#!/usr/bin/env bash
printf 'cppcheck %s\n' "$*" >> "$FAKE_TOOL_LOG"
exit "${FAKE_CPPCHECK_RC:-0}"
EOF
  cat > "$FAKE_BIN_DIR/timeout" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$*" >> "$FAKE_TIMEOUT_LOG"
while [[ "${1:-}" == --* ]]; do
  shift 2
done
duration="${1:?missing timeout duration}"
shift
tool="${1:?missing timed tool}"
tool_name="$(basename "$tool")"
if [[ "$tool_name" = "${FAKE_TIMEOUT_TOOL:-}" || "$tool_name" = "${FAKE_TIMEOUT_TOOL:-}"-* ]]; then
  exit 124
fi
exec "$@"
EOF
  chmod +x "$FAKE_BIN_DIR/xmake" "$FAKE_BIN_DIR/bc" "$FAKE_BIN_DIR/clang-tidy" \
    "$FAKE_BIN_DIR/cppcheck" "$FAKE_BIN_DIR/timeout"
  ln -s clang-tidy "$FAKE_BIN_DIR/clang-tidy-18"
  ln -s clang-tidy "$FAKE_BIN_DIR/clang-tidy-17"
  ln -s clang-tidy "$FAKE_BIN_DIR/clang-tidy-16"
}

run_lint() {
  local output_file="$1"
  shift
  set +e
  PATH="$FAKE_BIN_DIR:$PATH" FAKE_TIMEOUT_LOG="$FIXTURE_ROOT/timeout.log" \
    FAKE_TOOL_LOG="$FIXTURE_ROOT/tool.log" "$@" \
    bash "$FIXTURE_ROOT/scripts/lint.sh" src/example.cpp > "$output_file" 2>&1
  local rc=$?
  set -e
  return "$rc"
}

assert_timeout_wrapper() {
  local timeout_seconds="$1"
  local tool="$2"
  local log
  log="$(cat "$FIXTURE_ROOT/timeout.log")"
  assert_contains "$log" "--signal=TERM --kill-after=60s ${timeout_seconds}s $tool" \
    "$tool 必须使用指定 timeout 包装"
}

setup_fixture

: > "$FIXTURE_ROOT/timeout.log"
: > "$FIXTURE_ROOT/tool.log"
if ! run_lint "$FIXTURE_ROOT/default.log" env; then
  fail '默认 lint 夹具应通过'
fi
assert_timeout_wrapper 1800 clang-tidy
assert_timeout_wrapper 1800 cppcheck

for timeout_seconds in 600 7200; do
  : > "$FIXTURE_ROOT/timeout.log"
  if ! run_lint "$FIXTURE_ROOT/boundary-$timeout_seconds.log" \
    env CLANG_TIDY_TIMEOUT_SECONDS="$timeout_seconds" CPPCHECK_TIMEOUT_SECONDS="$timeout_seconds"; then
    fail "timeout=$timeout_seconds 应被 lint 接受"
  fi
  assert_timeout_wrapper "$timeout_seconds" clang-tidy
  assert_timeout_wrapper "$timeout_seconds" cppcheck
done

for variable in CLANG_TIDY_TIMEOUT_SECONDS CPPCHECK_TIMEOUT_SECONDS; do
  for value in 599 7201 invalid; do
    : > "$FIXTURE_ROOT/timeout.log"
    if run_lint "$FIXTURE_ROOT/invalid-$variable-$value.log" env "$variable=$value"; then
      fail "$variable=$value 必须在执行工具前被拒绝"
    fi
    output="$(cat "$FIXTURE_ROOT/invalid-$variable-$value.log")"
    assert_contains "$output" "$variable" 'lint timeout 错误必须说明变量名'
    assert_contains "$output" "$value" 'lint timeout 错误必须说明实际值'
    [ ! -s "$FIXTURE_ROOT/timeout.log" ] || fail '非法 timeout 不得调用 timeout 工具'
  done
done

: > "$FIXTURE_ROOT/timeout.log"
if run_lint "$FIXTURE_ROOT/clang-timeout.log" env FAKE_TIMEOUT_TOOL=clang-tidy; then
  fail 'clang-tidy timeout 必须使 lint 失败'
fi
assert_contains "$(cat "$FIXTURE_ROOT/clang-timeout.log")" 'clang-tidy 执行超时: src/example.cpp' \
  'clang-tidy timeout 必须说明目标文件'

: > "$FIXTURE_ROOT/timeout.log"
if run_lint "$FIXTURE_ROOT/cppcheck-timeout.log" env FAKE_TIMEOUT_TOOL=cppcheck; then
  fail 'cppcheck timeout 必须使 lint 失败'
fi
assert_contains "$(cat "$FIXTURE_ROOT/cppcheck-timeout.log")" 'cppcheck 执行超时: 本次扫描范围' \
  'cppcheck timeout 必须说明扫描范围'

printf 'Lint 脚本超时回归测试通过\n'
