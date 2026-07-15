#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

usage() {
  cat <<EOF
用法: $(basename "$0") [文件/目录...]

说明:
  对指定 C/C++ 源文件执行 clang-format 格式化
  省略参数时格式化所有源文件（排除 build/ .xmake/ compile_commands*）
  -h, --help    显示帮助

子命令:
  all           格式化所有源文件（默认）
  <路径...>     格式化指定文件/目录
EOF
}

detect_clang_format() {
  for cmd in clang-format-18 clang-format-17 clang-format-16 clang-format; do
    if command -v "$cmd" &>/dev/null; then
      echo "$cmd"
      return 0
    fi
  done
  return 1
}

cmd_format() {
  local fmt; fmt=$(detect_clang_format) || {
    yellow "clang-format 未安装，跳过"
    return
  }
  local targets=("$@")
  if [ ${#targets[@]} -eq 0 ]; then
    blue "=== 格式化全部源文件 ==="
    find "$PROJECT_ROOT" \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
      ! -path '*/build/*' ! -path '*/.xmake/*' ! -path '*/compile_commands*' \
      -exec "$fmt" -i {} + 2>&1
  else
    blue "=== 格式化指定文件 ==="
    local files=()
    for target in "${targets[@]}"; do
      if [ -f "$target" ]; then
        files+=("$target")
      elif [ -d "$target" ]; then
        while IFS= read -r f; do files+=("$f"); done < <(
          find "$target" \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
            ! -path '*/build/*' ! -path '*/.xmake/*'
        )
      else
        yellow "警告: 路径不存在: $target"
      fi
    done
    if [ ${#files[@]} -gt 0 ]; then
      printf '%s\0' "${files[@]}" | xargs -0 "$fmt" -i
    fi
  fi
  green "格式化完成"
}

handle_menu_choice() {
  case "$1" in
    1) cmd_format ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "all:格式化全部源文件"
  )
  menu_loop "格式化工具（$PROJECT_ROOT）" "${items[@]}"
}

if [ $# -gt 0 ]; then
  case "$1" in
    all) cmd_format ;;
    -h|--help) usage; exit 0 ;;
    *) cmd_format "$@" ;;
  esac
else
  menu
fi
