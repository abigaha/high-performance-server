#!/usr/bin/env bash
# ============================================================
# common.sh — 脚本公共库
# 用法: source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"
# ============================================================

if [ -n "${__COMMON_SH_LOADED:-}" ]; then
  return 0
fi
__COMMON_SH_LOADED=1

# 颜色函数
if [ -t 1 ]; then
  red()    { printf "\033[31m%s\033[0m\n" "$*"; }
  green()  { printf "\033[32m%s\033[0m\n" "$*"; }
  yellow() { printf "\033[33m%s\033[0m\n" "$*"; }
  blue()   { printf "\033[34m%s\033[0m\n" "$*"; }
else
  red()    { printf "%s\n" "$*"; }
  green()  { printf "%s\n" "$*"; }
  yellow() { printf "%s\n" "$*"; }
  blue()   { printf "%s\n" "$*"; }
fi

# 项目根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[1]:-$0}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 通用菜单渲染
# 用法: show_menu "标题" "选项1" "描述1" "选项2" "描述2" ...
show_menu() {
  local title="$1"; shift
  blue "===== $title ====="
  local i=1
  while [ $# -gt 0 ]; do
    local opt="$1"; shift
    local desc="${1:-}"; shift || true
    printf "%-2s) %s\n" "$i" "${desc:+$desc}"
    i=$((i+1))
  done
  printf "%-2s) %s\n" "q" "退出"
  printf "选择: "
}

# 通用菜单循环
# 用法: menu_loop "标题" "选项1:描述1" "选项2:描述2" ...
# 回调函数: handle_menu_choice "$choice" — 需由调用脚本定义
menu_loop() {
  local title="$1"; shift
  local items=("$@")
  while true; do
    echo
    local args=()
    for item in "${items[@]}"; do
      IFS=':' read -r opt desc <<< "$item"
      args+=("$opt" "$desc")
    done
    show_menu "$title" "${args[@]}"
    read -r choice
    echo
    if [ "$choice" = "q" ] || [ "$choice" = "Q" ]; then
      exit 0
    fi
    handle_menu_choice "$choice"
  done
}

# 检查命令是否存在
require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" &>/dev/null; then
    red "错误: $cmd 未安装"
    return 1
  fi
}
