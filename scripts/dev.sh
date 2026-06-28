#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

red()    { printf "\033[31m%s\033[0m\n" "$*"; }
green()  { printf "\033[32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[33m%s\033[0m\n" "$*"; }
blue()   { printf "\033[34m%s\033[0m\n" "$*"; }

cmd_compile() {
  blue "=== 编译 ==="
  if [ "${1:-}" = "--clean" ]; then
    xmake f -c -y && xmake -j"$(nproc)"
  else
    xmake -j"$(nproc)"
  fi
  green "编译成功"
}

cmd_format() {
  blue "=== 格式化 ==="
  local FMT=""
  for cmd in clang-format-18 clang-format-17 clang-format-16 clang-format; do
    if command -v "$cmd" &>/dev/null; then FMT="$cmd"; break; fi
  done
  if [ -z "$FMT" ]; then
    yellow "clang-format 未安装，跳过"
    return
  fi
  find . \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
    ! -path './build/*' ! -path './.xmake/*' ! -path './compile_commands*' \
    -exec "$FMT" -i {} + 2>&1
  green "格式化完成"
}

cmd_lint() {
  blue "=== Lint ==="
  bash "$ROOT/scripts/lint.sh"
}

cmd_test() {
  blue "=== 测试 ==="
  bash "$ROOT/scripts/test.sh"
}

cmd_codeql() {
  blue "=== CodeQL ==="
  if [ -z "${CODEQL_SERVER_URL:-}" ]; then
    yellow "CODEQL_SERVER_URL 未设置，尝试 http://localhost:8080"
    if curl -sf "http://localhost:8080/" >/dev/null 2>&1; then
      export CODEQL_SERVER_URL="http://localhost:8080"
    else
      read -r -p "请输入 CodeQL 服务器 IP（端口固定 8080）: " ip
      export CODEQL_SERVER_URL="http://${ip}:8080"
    fi
  fi
  green "CodeQL 服务器: $CODEQL_SERVER_URL"

  local tmpdir; tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' RETURN

  xmake project -k compile_commands
  python3 -c "
import json
with open('compile_commands.json') as f:
    data = json.load(f)
filtered = [e for e in data if not e['file'].startswith('/')]
for e in filtered:
    e['directory'] = '.'
    e['file'] = e['file'].replace('\\\\', '/')
with open('$tmpdir/compile_commands_fixed.json', 'w') as f:
    json.dump(filtered, f, indent=2)
print(f'过滤后保留 {len(filtered)}/{len(data)} 个编译条目')
"
  tar czf "$tmpdir/source.tar.gz" \
    --exclude='*.o' --exclude='*.obj' --exclude='*.exe' \
    --exclude='__pycache__' --exclude='.xmake' --exclude='build' \
    src/ tests/ xmake.lua core/ logger/ file-system/ memory-pool/ net/

  green "打包完成，发送到 CodeQL 服务器..."
  local resp; resp=$(curl -s -X POST "$CODEQL_SERVER_URL/analyze" \
    -F "source=@$tmpdir/source.tar.gz;type=application/gzip" \
    -F "compile_commands=@$tmpdir/compile_commands_fixed.json;type=application/json")

  local success; success=$(echo "$resp" | python3 -c "
import json,sys
try:
    r=json.load(sys.stdin)
    inv=r.get('result',{}).get('runs',[{}])[0].get('invocations',[{}])[0]
    ok=inv.get('executionSuccessful',False)
    results=r.get('result',{}).get('runs',[{}])[0].get('results',[])
    if not ok:
        print('FAIL')
    elif not results:
        print('PASS_ZERO')
    else:
        for res in results:
            rid=res.get('ruleId','?')
            sev=res.get('level','?')
            msg=res.get('message',{}).get('text','')
            loc=res.get('locations',[{}])[0].get('physicalLocation',{})
            art=loc.get('artifactLocation',{}).get('uri','?')
            rgn=loc.get('region',{}).get('startLine','?')
            print(f'{sev}:{rid} {art}:{rgn} {msg}')
except Exception as e:
    print(f'PARSE_ERR:{e}')
")
  case "$success" in
    PASS_ZERO) green "CodeQL 通过：0 critical + 0 high" ;;
    FAIL)      red "CodeQL 执行失败"; echo "$resp" | python3 -m json.tool 2>/dev/null || echo "$resp" ;;
    PARSE_ERR:*) yellow "解析响应失败: ${success#PARSE_ERR:}"; echo "$resp" ;;
    *)
      if echo "$success" | grep -qiE '\b(?:critical|high|error)'; then
        red "CodeQL 发现问题:"
        echo "$success"
      else
        yellow "CodeQL 结果:"
        echo "$success"
      fi
      ;;
  esac
}

cmd_all() {
  cmd_format
  cmd_lint
  cmd_compile
  cmd_test
}

menu() {
  while true; do
    echo
    blue "===== 开发工具（$ROOT）====="
    echo "1)  编译"
    echo "2)  格式化"
    echo "3)  Lint（clang-tidy + cppcheck）"
    echo "4)  测试"
    echo "5)  CodeQL"
    echo "6)  全部执行（格式→Lint→编译→测试）"
    echo "7)  编译（清缓存）"
    echo "q)  退出"
    printf "选择: "
    read -r choice
    echo
    case "$choice" in
      1) cmd_compile ;;
      2) cmd_format ;;
      3) cmd_lint ;;
      4) cmd_test ;;
      5) cmd_codeql ;;
      6) cmd_all ;;
      7) cmd_compile --clean ;;
      q|Q) exit 0 ;;
      *) red "无效选择" ;;
    esac
  done
}

if [ $# -gt 0 ]; then
  case "$1" in
    compile) shift; cmd_compile "$@" ;;
    format) cmd_format ;;
    lint) cmd_lint ;;
    test) cmd_test ;;
    codeql) cmd_codeql ;;
    all) cmd_all ;;
    *) echo "用法: $0 {compile|format|lint|test|codeql|all}"; exit 1 ;;
  esac
else
  menu
fi
