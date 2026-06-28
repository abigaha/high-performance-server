#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

red()    { printf "\033[31m%s\033[0m\n" "$*"; }
green()  { printf "\033[32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[33m%s\033[0m\n" "$*"; }

if [ -n "${1:-}" ]; then
  yellow "运行测试: $1"
  xmake test -f "$1" 2>&1
  echo ""
  green "测试完成"
  exit 0
fi

yellow "=== 全部测试 ==="
OUT=$(mktemp)
xmake test 2>&1 | tee "$OUT"

if grep -qi "nothing to test\|no test" "$OUT"; then
  yellow "暂无测试用例"
elif grep -qi "failed\|error\|FAILED" "$OUT"; then
  red "存在失败的测试"
  rm -f "$OUT"
  exit 1
else
  green "所有测试通过"
fi
rm -f "$OUT"
