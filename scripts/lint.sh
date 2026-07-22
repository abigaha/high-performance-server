#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"
ROOT="$PROJECT_ROOT"
cd "$PROJECT_ROOT"

HAS_ERROR=0
DEFAULT_JOBS="$(nproc 2>/dev/null || echo 4)"
JOBS="$DEFAULT_JOBS"
MODE="all"
PATHS=()

usage() {
  cat <<EOF
用法: $(basename "$0") [选项] [文件/目录...]
选项:
  --all            检查所有 C/C++ 源文件
  --changed        仅检查 Git 变更文件（相对 HEAD，含未跟踪）
  -j, --jobs N     并发数（默认 $DEFAULT_JOBS）
  -h, --help       显示帮助
说明:
  无参数时全量检查所有源文件（向后兼容）
  指定文件/目录时仅检查对应范围
  支持扩展名: .cpp .hpp .h .cc .cxx
EOF
}

detect_clang_tidy() {
  local cmd
  for cmd in clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy; do
    if command -v "$cmd" &>/dev/null; then
      printf '%s\n' "$cmd"
      return 0
    fi
  done
  return 1
}

check_required_tools() {
  require_cmd xmake
  require_cmd bc
  require_cmd cppcheck

  CLANG_TIDY="$(detect_clang_tidy)" || {
    red "错误: clang-tidy 未安装，无法执行严格 Lint 门禁"
    return 1
  }
}

run_frontend_lint() {
  yellow "=== 前端 Lint ==="
  if [ ! -d "$FRONTEND_DIR" ]; then
    yellow "未发现 frontend 目录，跳过前端 Lint"
    return 0
  elif ensure_frontend_dependencies && (cd "$FRONTEND_DIR" && npm run lint -- --deny-warnings); then
    green "前端 Lint 通过"
  else
    red "前端 Lint 失败"
    HAS_ERROR=1
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --all) MODE="all"; shift ;;
    --changed) MODE="changed"; shift ;;
    -j|--jobs) JOBS="${2:?缺少并发数}"; shift 2 ;;
    -j*) JOBS="${1#-j}"; shift ;;
    --jobs=*) JOBS="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; MODE="paths"; while [ $# -gt 0 ]; do PATHS+=("$1"); shift; done ;;
    -*) red "未知选项: $1"; usage; exit 2 ;;
    *) MODE="paths"; PATHS+=("$1"); shift ;;
  esac
done

if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [ "$JOBS" -lt 1 ]; then
  red "并发数无效: $JOBS"
  exit 2
fi

CLANG_TIDY=""
check_required_tools

if [ ! -s compile_commands.json ]; then
  yellow "compile_commands.json 不存在，自动生成..."
  xmake project -k compile_commands
else
  XMAKE_MTIMES=$(find . -name 'xmake.lua' -type f \
    ! -path './build/*' ! -path './.xmake/*' -printf '%T@\n' | sort -rn)
  XMAKE_NEWEST="${XMAKE_MTIMES%%$'\n'*}"
  DB_MTIME=$(stat -L -c '%Y' compile_commands.json 2>/dev/null || echo 0)
  if [ -n "$XMAKE_NEWEST" ] && \
     [ "$(printf '%s > %s\n' "$XMAKE_NEWEST" "$DB_MTIME" | bc)" -eq 1 ]; then
    yellow "xmake.lua 已更新，重新生成 compile_commands.json..."
    xmake project -k compile_commands
  fi
fi

if [ ! -s compile_commands.json ]; then
  red "错误: 未生成有效的 compile_commands.json"
  exit 1
fi

collect_all_sources() {
  find . \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \
    -o -name '*.cc' -o -name '*.cxx' \) \
    ! -path './build/*' ! -path './.xmake/*' ! -path './compile_commands*' \
    ! -path './.git/*' ! -path './.opencode/*' ! -path '*/node_modules/*' \
    | sort
}

collect_path_sources() {
  local p
  for p in "${PATHS[@]}"; do
    if [ -f "$p" ]; then
      case "$p" in
        *.cpp|*.hpp|*.h|*.cc|*.cxx) echo "$p" ;;
      esac
    elif [ -d "$p" ]; then
      find "$p" \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \
        -o -name '*.cc' -o -name '*.cxx' \) \
        ! -path '*/build/*' ! -path '*/.xmake/*' ! -path '*/.git/*' \
        ! -path '*/.opencode/*' ! -path '*/node_modules/*'
    else
      yellow "警告: 路径不存在: $p"
    fi
  done | sort -u
}

collect_changed_sources() {
  if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    red "错误: --changed 需要 Git 仓库" >&2
    return 2
  fi

  local changed_files
  local untracked_files
  if ! changed_files=$(git diff --name-only --diff-filter=ACMRTUXB HEAD); then
    red "错误: 无法读取 Git 变更文件" >&2
    return 2
  fi
  if ! untracked_files=$(git ls-files --others --exclude-standard); then
    red "错误: 无法读取 Git 未跟踪文件" >&2
    return 2
  fi

  printf '%s\n%s\n' "$changed_files" "$untracked_files" \
    | awk '/\.(cpp|hpp|h|cc|cxx)$/' | sort -u
}

case "$MODE" in
  all)     SOURCES=$(collect_all_sources) ;;
  paths)   SOURCES=$(collect_path_sources) ;;
  changed) SOURCES=$(collect_changed_sources) ;;
esac

if [ -z "$SOURCES" ]; then
  green "无需检查的 C++ 源文件"
  run_frontend_lint
  echo ""
  if [ "$HAS_ERROR" -eq 0 ]; then
    green "=== Lint 全部通过 ==="
  else
    red "=== Lint 发现问题，请修复 ==="
  fi
  exit "$HAS_ERROR"
fi

CPP_FILES=$(printf '%s\n' "$SOURCES" | grep -E '\.(cpp|cc|cxx)$' || true)

echo ""
yellow "=== clang-tidy ==="
if [ -z "$CPP_FILES" ]; then
  green "clang-tidy: 无 .cpp/.cc/.cxx 文件可检查"
else
  TIDY_DIR=$(mktemp -d)
  TIDY_FAILED="$TIDY_DIR/failed"
  : > "$TIDY_FAILED"

  PROJECT_ROOT="$ROOT"
  printf '%s\n' "$CPP_FILES" | awk '{ printf "%d\t%s\n", NR, $0 }' | \
    while IFS=$'\t' read -r idx f; do printf '%s\0%s\0' "$idx" "$f"; done | \
    xargs -0 -P "$JOBS" -n 2 bash -c '
      idx="$1"; f="$2"
      outdir="'"$TIDY_DIR"'"; cmd="'"$CLANG_TIDY"'"; proot="'"$PROJECT_ROOT"'"
      args=(--quiet --warnings-as-errors="*" --header-filter="${proot}/.*" -p .)
      if [[ "$f" == tests/* ]] && [ -f "${proot}/tests/.clang-tidy" ]; then
        args+=(--config-file="${proot}/tests/.clang-tidy")
      elif [[ "$f" == benchmark/* ]] && [ -f "${proot}/benchmark/.clang-tidy" ]; then
        args+=(--config-file="${proot}/benchmark/.clang-tidy")
      fi
      if ! "$cmd" "${args[@]}" "$f" > "$outdir/$idx.out" 2>&1; then
        printf "%s\n" "$f" >> "$outdir/failed"
      fi
    ' _

  TIDY_OUT=$(mktemp)
  max_idx=$(printf '%s\n' "$CPP_FILES" | awk 'END { print NR }')
  i=1
  while [ "$i" -le "$max_idx" ]; do
    [ -f "$TIDY_DIR/$i.out" ] && cat "$TIDY_DIR/$i.out" >> "$TIDY_OUT"
    i=$((i+1))
  done

  if [ -s "$TIDY_OUT" ]; then
    cat "$TIDY_OUT"
  fi
  if [ -s "$TIDY_FAILED" ]; then
    red "clang-tidy 发现问题或执行失败，涉及文件:"
    cat "$TIDY_FAILED"
    HAS_ERROR=1
  else
    green "clang-tidy: 0 error, 0 warning, 0 style"
  fi
  rm -rf "$TIDY_DIR" "$TIDY_OUT"
fi

echo ""
yellow "=== cppcheck ==="
mapfile -t INCLUDE_DIRS < <(find . \( -name '*.h' -o -name '*.hpp' \) \
  ! -path './build/*' ! -path './.xmake/*' ! -path './compile_commands*' \
  ! -path './.git/*' ! -path './.opencode/*' ! -path '*/node_modules/*' \
  -exec dirname {} \; | sort -u)
INCLUDE_ARGS=()
for include_dir in "${INCLUDE_DIRS[@]}"; do
  INCLUDE_ARGS+=("-I" "$include_dir")
done
mapfile -t SOURCE_FILES <<< "$SOURCES"

CPP_OUT=$(mktemp)
# cppcheck 无法可靠追踪公开头文件成员在其他翻译单元或外部调用方中的使用情况，
# 局部与全量模式统一抑制这一固有限制；其余 error/warning/style/performance 仍由 --enable=all 严格检查。
# 局部目标未触发该规则时，命令行范围抑制会产生 unmatchedSuppression；checkLevelNormal 与
# checkersReport 同样是检查范围/运行状态元信息。仅忽略这三项，避免 --error-exitcode 将元信息误判为门禁失败。
CPPCHECK_POLICY_ARGS=(
  --disable=unusedFunction
  --suppress=unusedStructMember
  --suppress=unmatchedSuppression
  --suppress=checkLevelNormal
  --suppress=checkersReport
)
set +e
# Google Test 宏需要 cppcheck 官方 googletest 模型才能正确展开；该模型只补充解析语义，
# 不抑制 syntaxError，也不放宽 error/warning/style/performance 门禁。
cppcheck --enable=all --error-exitcode=1 \
  --library=googletest \
  "${CPPCHECK_POLICY_ARGS[@]}" --suppress=missingIncludeSystem \
  --inline-suppr --language=c++ --std=c++20 \
  "${INCLUDE_ARGS[@]}" "${SOURCE_FILES[@]}" >"$CPP_OUT" 2>&1
CPPCHECK_RC=$?
set -e

if [ -s "$CPP_OUT" ]; then
  cat "$CPP_OUT"
fi
if [ "$CPPCHECK_RC" -ne 0 ]; then
  red "cppcheck 发现问题或执行失败"
  HAS_ERROR=1
else
  green "cppcheck: 0 error, 0 warning, 0 style, 0 performance"
fi
rm -f "$CPP_OUT"

run_frontend_lint
echo ""
if [ "$HAS_ERROR" -eq 0 ]; then
  green "=== Lint 全部通过 ==="
else
  red "=== Lint 发现问题，请修复 ==="
fi
exit "$HAS_ERROR"
