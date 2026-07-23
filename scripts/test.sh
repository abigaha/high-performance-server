#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"
cd "$PROJECT_ROOT"

run_frontend_tests() {
  yellow "=== 前端 Vitest ==="
  if [ ! -d "$FRONTEND_DIR" ]; then
    yellow "未发现 frontend 目录，跳过前端 Vitest"
    return 0
  fi

  ensure_frontend_dependencies
  (
    cd "$FRONTEND_DIR"
    npm run test
  )
  green "前端 Vitest 通过"
}

run_script_regression_tests() {
  yellow "=== 脚本回归测试 ==="
  bash "$PROJECT_ROOT/tests/test_codeql_discovery.sh"
  green "脚本回归测试通过"
}


if [ -n "${1:-}" ]; then
  test_selector="$1"
  if [[ "$test_selector" != */* ]]; then
    test_selector="${test_selector}/default"
  fi

  yellow "运行测试: $test_selector"
  xmake test "$test_selector" 2>&1
  run_script_regression_tests
  run_frontend_tests
  echo ""
  green "测试完成"
  exit 0
fi

yellow "=== 全部测试 ==="
OUT=$(mktemp)
# 用 xmake 退出码做主判断，不依赖带颜色码的文本匹配
set +e
xmake test -j1 2>&1 | tee "$OUT"
XMAKE_RC=${PIPESTATUS[0]}
set -e

if [ "$XMAKE_RC" -ne 0 ]; then
  red "存在失败的测试"
  rm -f "$OUT"
  exit 1
elif grep -qi "nothing to test\|no test" "$OUT"; then
  yellow "暂无测试用例"
else
  green "所有测试通过"
fi
rm -f "$OUT"

run_script_regression_tests
run_frontend_tests
echo ""
green "测试完成"
