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
  bash "$ROOT/scripts/lint.sh" "$@"
}

cmd_test() {
  blue "=== 测试 ==="
  bash "$ROOT/scripts/test.sh"
}

cmd_codeql() {
  blue "=== CodeQL ==="
  if [ -z "${CODEQL_SERVER_URL:-}" ]; then
    yellow "CODEQL_SERVER_URL 未设置，尝试 http://localhost:8080"
    if curl -s --connect-timeout 5 -o /dev/null "http://localhost:8080/" 2>/dev/null; then
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
    tests/ xmake.lua core/ logger/ file-system/ memory-pool/ net/ db/

  green "打包完成，发送到 CodeQL 服务器..."
  yellow "分析中（可能耗时 1-5 分钟，请耐心等待）..."
  local resp; resp=$(curl -s --max-time 300 -X POST "$CODEQL_SERVER_URL/analyze" \
    -F "source=@$tmpdir/source.tar.gz;type=application/gzip" \
    -F "compile_commands=@$tmpdir/compile_commands_fixed.json;type=application/json") || {
    red "CodeQL 请求失败（curl 退出码 $?），可能原因："
    red "  - 服务器地址 $CODEQL_SERVER_URL 无法访问"
    red "  - 分析超时（>300 秒）"
    red "  - 服务器内部错误"
    return 1
  }

  if [ -z "$resp" ]; then
    red "CodeQL 返回空响应"
    return 1
  fi

  local result_file; result_file="$tmpdir/codeql_result.txt"
  echo "$resp" | python3 -c "
import json,sys
try:
    r=json.load(sys.stdin)
    runs=r.get('result',{}).get('runs',[])
    inv=runs[0].get('invocations',[{}])[0] if runs else {}
    ok=inv.get('executionSuccessful',False)
    if not ok:
        print('EXEC_FAIL')
        for n in inv.get('toolExecutionNotifications',[]):
            m=n.get('message',{}).get('text','')
            if m: print(f'  {m}')
        sys.exit(0)
    results=runs[0].get('results',[]) if runs else []
    crit=0
    high=0
    for res in results:
        rid=res.get('ruleId','?')
        sev=res.get('level','?')
        msg=res.get('message',{}).get('text','')
        loc=res.get('locations',[{}])[0].get('physicalLocation',{})
        art=loc.get('artifactLocation',{}).get('uri','?')
        rgn=loc.get('region',{}).get('startLine','?')
        print(f'{sev}:{rid} {art}:{rgn} {msg}')
        if sev == 'error': crit+=1
        elif sev == 'warning': high+=1
    print(f'__CRITICAL__={crit}')
    print(f'__HIGH__={high}')
except Exception as e:
    print(f'PARSE_ERR:{e}')
" > "$result_file"

  if grep -q '^EXEC_FAIL' "$result_file"; then
    red "CodeQL 执行失败"
    cat "$result_file"
    return 1
  fi
  if grep -q '^PARSE_ERR' "$result_file"; then
    yellow "解析 CodeQL 响应失败: $(grep '^PARSE_ERR' "$result_file" | sed 's/^PARSE_ERR://')"
    yellow "原始响应（前 2000 字符）:"
    echo "$resp" | head -c 2000
    echo ""
    return 1
  fi

  local critical; critical=$(grep -oP '__CRITICAL__=\K\d+' "$result_file" 2>/dev/null || echo 0)
  local high; high=$(grep -oP '__HIGH__=\K\d+' "$result_file" 2>/dev/null || echo 0)

  # 打印所有发现
  grep -v '^__' "$result_file" | while IFS= read -r line; do
    case "$line" in
      error:*) red "$line" ;;
      warning:*) yellow "$line" ;;
      *) echo "$line" ;;
    esac
  done

  if [ "$critical" -gt 0 ] || [ "$high" -gt 0 ]; then
    red "CodeQL 门禁未通过: critical=$critical, high=$high"
    return 1
  else
    green "CodeQL 通过：0 critical + 0 high"
  fi
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
    lint) shift; cmd_lint "$@" ;;
    test) cmd_test ;;
    codeql) cmd_codeql ;;
    all) cmd_all ;;
    *) echo "用法: $0 {compile|format|lint|test|codeql|all}"; exit 1 ;;
  esac
else
  menu
fi
