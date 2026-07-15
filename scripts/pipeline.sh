#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

usage() {
  cat <<EOF
用法: $(basename "$0") [子命令]

子命令:
  all             完整流水线：格式 → Lint → 编译 → 测试（默认）
  format          仅格式
  lint            仅 Lint
  compile         仅编译
  test            仅测试
  -h, --help      显示帮助

说明:
  无参数时进入交互菜单
EOF
}

cmd_all() {
  blue "=== 流水线开始 ==="
  echo ""
  bash "$PROJECT_ROOT/scripts/format.sh" all
  echo ""
  bash "$PROJECT_ROOT/scripts/lint.sh"
  echo ""
  bash "$PROJECT_ROOT/scripts/compile.sh" build
  echo ""
  bash "$PROJECT_ROOT/scripts/test.sh"
  echo ""
  green "=== 流水线完成 ==="
}

handle_menu_choice() {
  case "$1" in
    1) cmd_all ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "all:完整流水线（格式 → Lint → 编译 → 测试）"
  )
  menu_loop "流水线工具（$PROJECT_ROOT）" "${items[@]}"
}

if [ $# -gt 0 ]; then
  case "$1" in
    all) cmd_all ;;
    format) bash "$PROJECT_ROOT/scripts/format.sh" all ;;
    lint) bash "$PROJECT_ROOT/scripts/lint.sh" ;;
    compile) bash "$PROJECT_ROOT/scripts/compile.sh" build ;;
    test) bash "$PROJECT_ROOT/scripts/test.sh" ;;
    -h|--help) usage; exit 0 ;;
    *) red "未知子命令: $1"; usage; exit 1 ;;
  esac
else
  menu
fi
