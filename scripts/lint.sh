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
  --changed        仅检查 Git 变更文件（相对 HEAD，含未跟踪）
  -j, --jobs N     并发数（默认 $DEFAULT_JOBS）
  -h, --help       显示帮助
说明:
  无参数时全量检查所有源文件（向后兼容）
  指定文件/目录时仅检查对应范围
EOF
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

if [ ! -f compile_commands.json ]; then
  yellow "compile_commands.json 不存在，自动生成..."
  xmake project -k compile_commands
else
  XMAKE_NEWEST=$(find . -name 'xmake.lua' -type f -printf '%T@\n' | sort -rn | head -1)
  DB_MTIME=$(stat -c '%Y' compile_commands.json 2>/dev/null || echo 0)
  if [ "$(echo "$XMAKE_NEWEST > $DB_MTIME" | bc)" -eq 1 ]; then
    yellow "xmake.lua 已更新，重新生成 compile_commands.json..."
    xmake project -k compile_commands
  fi
fi

collect_all_sources() {
  find . \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
    ! -path './build/*' ! -path './.xmake/*' ! -path './compile_commands*' \
    ! -path './.opencode/*' \
    | sort
}

collect_path_sources() {
  local p
  for p in "${PATHS[@]}"; do
    if [ -f "$p" ]; then
      case "$p" in
        *.cpp|*.hpp|*.h) echo "$p" ;;
      esac
    elif [ -d "$p" ]; then
      find "$p" \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
        ! -path '*/build/*' ! -path '*/.xmake/*' ! -path '*/.opencode/*'
    else
      yellow "警告: 路径不存在: $p"
    fi
  done | sort -u
}

collect_changed_sources() {
  if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    red "错误: --changed 需要 Git 仓库"
    exit 2
  fi
  { git diff --name-only HEAD 2>/dev/null; git ls-files --others --exclude-standard 2>/dev/null; } \
    | grep -E '\.(cpp|hpp|h)$' | sort -u
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

CPP_FILES=$(echo "$SOURCES" | grep '\.cpp$' || true)

echo ""
yellow "=== clang-tidy ==="
CLANG_TIDY=""
for cmd in clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy; do
  if command -v "$cmd" &>/dev/null; then CLANG_TIDY="$cmd"; break; fi
done
if [ -z "$CLANG_TIDY" ]; then
  yellow "clang-tidy 未安装，跳过"
else
  if [ -z "$CPP_FILES" ]; then
    green "clang-tidy: 无 .cpp 文件可检查，跳过"
  else
    TIDY_DIR=$(mktemp -d)
    TIDY_FAILED="$TIDY_DIR/failed"
    : > "$TIDY_FAILED"

    PROJECT_ROOT="$ROOT"
    echo "$CPP_FILES" | grep . | awk '{ printf "%d\t%s\n", NR, $0 }' | \
      while IFS=$'\t' read -r idx f; do printf '%s\0%s\0' "$idx" "$f"; done | \
      xargs -0 -P "$JOBS" -n 2 bash -c '
        idx="$1"; f="$2"
        outdir="'"$TIDY_DIR"'"; cmd="'"$CLANG_TIDY"'"; proot="'"$PROJECT_ROOT"'"
        cfg=""
        if [[ "$f" == tests/* ]] && [ -f "${proot}/tests/.clang-tidy" ]; then
          cfg="--config-file=${proot}/tests/.clang-tidy"
        elif [[ "$f" == benchmark/* ]] && [ -f "${proot}/benchmark/.clang-tidy" ]; then
          cfg="--config-file=${proot}/benchmark/.clang-tidy"
        fi
        if ! "$cmd" --quiet $cfg --header-filter="${proot}/.*" -p . "$f" > "$outdir/$idx.out" 2>&1; then
          echo "$f" >> "$outdir/failed"
        fi
      ' _

    TIDY_OUT=$(mktemp)
    max_idx=$(echo "$CPP_FILES" | grep -c .)
    i=1
    while [ "$i" -le "$max_idx" ]; do
      [ -f "$TIDY_DIR/$i.out" ] && cat "$TIDY_DIR/$i.out" >> "$TIDY_OUT"
      i=$((i+1))
    done

    TIDY_OK=true
    if [ -s "$TIDY_FAILED" ]; then TIDY_OK=false; fi

    TIDY_RESULTS=$(grep -v 'warnings generated\.' "$TIDY_OUT" | grep -v '^$' | grep -v '\.xmake/packages/' | grep -v '/usr/' || true)
    if [ -n "$TIDY_RESULTS" ]; then
      red "clang-tidy 发现问题:"
      echo "$TIDY_RESULTS"
      HAS_ERROR=1
    elif $TIDY_OK; then
      green "clang-tidy: 0 error, 0 warning"
    else
      yellow "clang-tidy: 执行异常（检查配置）"
      HAS_ERROR=1
    fi
    rm -rf "$TIDY_DIR" "$TIDY_OUT"
  fi
fi

echo ""
yellow "=== cppcheck ==="
INCLUDES=$(find . \( -name '*.h' -o -name '*.hpp' \) \
  ! -path './build/*' ! -path './.xmake/*' ! -path './compile_commands*' \
  ! -path './.opencode/*' \
  -exec dirname {} \; | sort -u | sed 's/^/-I /' | tr '\n' ' ')

CPP_OUT=$(mktemp)
cppcheck --enable=all --suppress=unusedFunction --suppress=unusedStructMember --suppress=missingIncludeSystem \
  --inline-suppr --language=c++ --std=c++20 \
  $INCLUDES $SOURCES 2>"$CPP_OUT" || true

CPP_RESULTS=$(grep -E ':(error|warning|style|performance):' "$CPP_OUT" 2>/dev/null || true)
if [ -n "$CPP_RESULTS" ]; then
  red "cppcheck 发现问题:"
  echo "$CPP_RESULTS"
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
