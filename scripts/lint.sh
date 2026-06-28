#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

red()    { printf "\033[31m%s\033[0m\n" "$*"; }
green()  { printf "\033[32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[33m%s\033[0m\n" "$*"; }

HAS_ERROR=0

# --- 检查 compile_commands.json ---
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

# --- 收集源文件 ---
SOURCES=$(find . \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
  ! -path './build/*' ! -path './.xmake/*' ! -path './compile_commands*' \
  | sort)

CPP_FILES=$(echo "$SOURCES" | grep '\.cpp$')

# --- clang-tidy ---
echo ""
yellow "=== clang-tidy ==="
CLANG_TIDY=""
for cmd in clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy; do
  if command -v "$cmd" &>/dev/null; then CLANG_TIDY="$cmd"; break; fi
done
if [ -z "$CLANG_TIDY" ]; then
  yellow "clang-tidy 未安装，跳过"
else
  TIDY_OK=true
  TIDY_OUT=$(mktemp)
  while IFS= read -r f; do
    if ! "$CLANG_TIDY" --quiet "$f" 1>>"$TIDY_OUT" 2>&1; then
      TIDY_OK=false
    fi
  done <<< "$CPP_FILES"

  # 过滤掉标准库头文件的 summary 行
  TIDY_RESULTS=$(grep -v 'warnings generated\.' "$TIDY_OUT" | grep -v '^$' || true)
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
  rm -f "$TIDY_OUT"
fi

# --- cppcheck ---
echo ""
yellow "=== cppcheck ==="
CPP_OUT=$(mktemp)
cppcheck --enable=all --suppress=unusedFunction --suppress=unusedStructMember --suppress=missingIncludeSystem \
  --language=c++ --std=c++20 \
  -I core -I logger/include -I memory-pool/include -I file-system/include \
  -I net/coroutine -I net/thread-pool/include \
  -I net/tcp/ctcpclient/include -I net/tcp/ctcpserver/include \
  $SOURCES 2>"$CPP_OUT" || true

if grep -q 'error\|warning\|style\|performance' "$CPP_OUT" 2>/dev/null; then
  red "cppcheck 发现问题:"
  cat "$CPP_OUT"
  HAS_ERROR=1
else
  green "cppcheck: 0 error, 0 warning, 0 style, 0 performance"
fi
rm -f "$CPP_OUT"

# --- 汇总 ---
echo ""
if [ "$HAS_ERROR" -eq 0 ]; then
  green "=== Lint 全部通过 ==="
else
  red "=== Lint 发现问题，请修复 ==="
fi
exit "$HAS_ERROR"
