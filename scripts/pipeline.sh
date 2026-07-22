#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

usage() {
  cat <<EOF
用法: $(basename "$0") [子命令]

子命令:
  all             完整流水线：格式化 → Lint → 编译 → CodeQL → 测试
  format          仅格式化
  lint            仅 Lint
  compile         仅编译
  codeql          仅 CodeQL 分析
  test            仅测试
  -h, --help      显示帮助

说明:
  无参数时进入交互菜单
EOF
}

run_format() {
  bash "$PROJECT_ROOT/scripts/format.sh" all
}

run_lint() {
  bash "$PROJECT_ROOT/scripts/lint.sh" --all
}

run_compile() {
  bash "$PROJECT_ROOT/scripts/compile.sh" build
}

run_codeql() {
  bash "$PROJECT_ROOT/scripts/codeql.sh" run
}

run_test() {
  # test.sh 的既有接口以无参数表示全量测试。
  bash "$PROJECT_ROOT/scripts/test.sh"
}

cmd_all() {
  blue "=== 流水线开始 ==="
  echo ""
  run_format
  echo ""
  run_lint
  echo ""
  run_compile
  echo ""
  run_codeql
  echo ""
  run_test
  echo ""
  green "=== 流水线完成 ==="
}

handle_menu_choice() {
  case "$1" in
    1) cmd_all ;;
    2) run_format ;;
    3) run_lint ;;
    4) run_compile ;;
    5) run_codeql ;;
    6) run_test ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "all:完整流水线（格式化 → Lint → 编译 → CodeQL → 测试）"
    "format:仅格式化"
    "lint:仅 Lint"
    "compile:仅编译"
    "codeql:仅 CodeQL 分析"
    "test:仅测试"
  )
  menu_loop "流水线工具（$PROJECT_ROOT）" "${items[@]}"
}

if [ $# -gt 0 ]; then
  case "$1" in
    all) cmd_all ;;
    format) run_format ;;
    lint) run_lint ;;
    compile) run_compile ;;
    codeql) run_codeql ;;
    test) run_test ;;
    -h|--help) usage; exit 0 ;;
    *) red "未知子命令: $1"; usage; exit 1 ;;
  esac
else
  menu
fi
