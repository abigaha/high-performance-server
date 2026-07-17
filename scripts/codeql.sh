#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

usage() {
  cat <<EOF
用法: $(basename "$0") [选项]

子命令:
  run             运行 CodeQL 分析
  -h, --help      显示帮助

说明:
  无参数时进入交互菜单
EOF
}

discover_server() {
  if [ -n "${CODEQL_SERVER_URL:-}" ]; then
    return 0
  fi
  yellow "CODEQL_SERVER_URL 未设置，尝试 http://localhost:8080"
  if curl -s --connect-timeout 5 -o /dev/null "http://localhost:8080/" 2>/dev/null; then
    export CODEQL_SERVER_URL="http://localhost:8080"
  else
    read -r -p "请输入 CodeQL 服务器 IP（端口固定 8080）: " ip
    export CODEQL_SERVER_URL="http://${ip}:8080"
  fi
  green "CodeQL 服务器: $CODEQL_SERVER_URL"
}

cmd_run() {
  blue "=== CodeQL ==="
  discover_server

  local tmpdir; tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' RETURN

  cd "$PROJECT_ROOT"

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
  local resp; resp=$(curl -s --max-time 600 -X POST "$CODEQL_SERVER_URL/analyze" \
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

handle_menu_choice() {
  case "$1" in
    1) cmd_run ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "run:运行 CodeQL 分析"
  )
  menu_loop "CodeQL 工具（$PROJECT_ROOT）" "${items[@]}"
}

if [ $# -gt 0 ]; then
  case "$1" in
    run|analyze) cmd_run ;;
    -h|--help) usage; exit 0 ;;
    *) red "未知选项: $1"; usage; exit 1 ;;
  esac
else
  menu
fi
