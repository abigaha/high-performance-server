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

probe_server() {
  local server_url="$1"
  local connect_timeout="$2"
  local request_timeout="$3"
  local http_code

  http_code=$(curl -sS --connect-timeout "$connect_timeout" --max-time "$request_timeout" \
    -o /dev/null -w '%{http_code}' "${server_url}/" 2>/dev/null) || return 1
  [[ "$http_code" =~ ^[1-5][0-9][0-9]$ ]]
}

discover_server() {
  local connect_timeout="${CODEQL_CONNECT_TIMEOUT:-3}"
  local request_timeout="${CODEQL_DISCOVERY_TIMEOUT:-5}"

  if [ -n "${CODEQL_SERVER_URL:-}" ]; then
    green "CodeQL 服务器: $CODEQL_SERVER_URL"
    return 0
  fi
  yellow "CODEQL_SERVER_URL 未设置，尝试 http://localhost:8080"
  if probe_server "http://localhost:8080" "$connect_timeout" "$request_timeout"; then
    export CODEQL_SERVER_URL="http://localhost:8080"
  else
    if [ ! -t 0 ]; then
      red "错误: CodeQL 服务不可达，非交互环境请设置 CODEQL_SERVER_URL"
      return 1
    fi

    while true; do
      if ! read -r -p "请输入 CodeQL 服务器 IP（端口固定 8080，直接回车取消）: " ip; then
        red "已取消 CodeQL 服务器输入"
        return 1
      fi
      if [ -z "$ip" ]; then
        yellow "已取消 CodeQL 分析"
        return 1
      fi

      export CODEQL_SERVER_URL="http://${ip}:8080"
      yellow "尝试连接 $CODEQL_SERVER_URL"
      if probe_server "$CODEQL_SERVER_URL" "$connect_timeout" "$request_timeout"; then
        break
      fi
      red "无法连接 $CODEQL_SERVER_URL，请重试"
    done
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
    tests/ xmake.lua core/ logger/ file-system/ memory-pool/ net/ db/ benchmark/

  local resp_file; resp_file="$tmpdir/post_resp.json"

  green "打包完成，发送到 CodeQL 服务器..."
  yellow "分析中（可能耗时 1-5 分钟，请耐心等待）..."

  # 提交分析，--max-time 30 仅等待请求被接受，不等待分析完成
  set +e
  curl -s --max-time 30 -X POST "$CODEQL_SERVER_URL/analyze" \
    -F "source=@$tmpdir/source.tar.gz;type=application/gzip" \
    -F "compile_commands=@$tmpdir/compile_commands_fixed.json;type=application/json" \
    -o "$resp_file"
  local curl_exit=$?
  set -e

  local task_id=""
  if [ "$curl_exit" -eq 0 ] && [ -s "$resp_file" ]; then
    task_id=$(python3 -c "
import json
r=json.load(open('$resp_file'))
print(r.get('task_id',''))
" 2>/dev/null || echo "")
  fi

  if [ -z "$task_id" ]; then
    yellow "从 Docker 容器获取最新任务 ID..."
    task_id=$(docker exec codeql-server ls -1t /data/uploads/ 2>/dev/null | head -1 || echo "")
    if [ -z "$task_id" ]; then
      red "无法获取任务 ID"
      return 1
    fi
  fi

  green "任务 ID: $task_id"

  # 轮询 /result/<task_id> 直到就绪
  yellow "轮询分析结果..."
  for i in $(seq 1 120); do
    sleep 5
    if curl -s --max-time 10 -o "$resp_file" "$CODEQL_SERVER_URL/result/$task_id" 2>/dev/null && [ -s "$resp_file" ]; then
      if python3 -c "
import json
r=json.load(open('$resp_file'))
if 'result' in r: r=r['result']
ok=r.get('runs',[{}])[0].get('invocations',[{}])[0].get('executionSuccessful',False)
exit(0 if ok else 1)
" 2>/dev/null; then
        green "结果就绪"
        parse_sarif "$resp_file"
        return $?
      fi
    fi
    if [ "$i" -eq 120 ]; then
      red "轮询超时（600 秒）"
      return 1
    fi
  done
}

parse_sarif() {
  local input="$1"
  local result_file="$tmpdir/codeql_result.txt"

  python3 -c "
import json,sys
try:
    r=json.load(open('$input'))
    if 'result' in r:
        r = r['result']
    runs=r.get('runs',[])
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
        level=str(res.get('level','')).lower()
        props=res.get('properties') or {}
        if not isinstance(props, dict):
            props={}
        severity=str(props.get('severity','')).lower()
        gate_severity=''
        if level == 'error' or severity in ('critical', 'error'):
            gate_severity='critical'
            crit+=1
        elif level == 'warning' or severity in ('high', 'warning'):
            gate_severity='high'
            high+=1
        sev=gate_severity or severity or level or '?'
        msg=res.get('message',{}).get('text','')
        loc=res.get('locations',[{}])[0].get('physicalLocation',{})
        art=loc.get('artifactLocation',{}).get('uri','?')
        rgn=loc.get('region',{}).get('startLine','?')
        print(f'{sev}:{rid} {art}:{rgn} {msg}')
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
    return 1
  fi

  local critical; critical=$(grep -oP '__CRITICAL__=\K\d+' "$result_file" 2>/dev/null || echo 0)
  local high; high=$(grep -oP '__HIGH__=\K\d+' "$result_file" 2>/dev/null || echo 0)

  grep -v '^__' "$result_file" 2>/dev/null || true

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
