#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

usage() {
  cat <<EOF
用法: $(basename "$0") [选项]

子命令:
  build             编译项目（默认）
  --clean           清缓存后编译
  -h, --help        显示帮助

说明:
  无参数时进入交互菜单
EOF
}

cmd_build() {
  blue "=== 编译 ==="
  if [ "${1:-}" = "--clean" ]; then
    xmake f -c -y && xmake -j"$(nproc)"
  else
    xmake -j"$(nproc)"
  fi
  green "编译成功"
}

handle_menu_choice() {
  case "$1" in
    1) cmd_build ;;
    2) cmd_build --clean ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "build:编译项目"
    "clean:编译（清缓存）"
  )
  menu_loop "编译工具（$PROJECT_ROOT）" "${items[@]}"
}

if [ $# -gt 0 ]; then
  case "$1" in
    build) shift; cmd_build "$@" ;;
    --clean) cmd_build --clean ;;
    -h|--help) usage; exit 0 ;;
    *) red "未知选项: $1"; usage; exit 1 ;;
  esac
else
  menu
fi
