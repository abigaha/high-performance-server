#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"

COMPILE_DATABASE="$PROJECT_ROOT/compile_commands.json"
CODEQL_TASK_ID=""

usage() {
  cat <<EOF
用法: $(basename "$0") [选项]

子命令:
  run             运行 CodeQL 分析
  analyze         run 的兼容别名
  -h, --help      显示帮助

环境变量:
  CODEQL_SUBMIT_TIMEOUT  提交请求超时秒数，默认 300，必须为正整数

说明:
  无参数时进入交互菜单
EOF
}

check_required_tools() {
  require_cmd curl
  require_cmd python3
  require_cmd readlink
  require_cmd tar
  require_cmd xmake
}

probe_server() {
  local server_url="${1%/}"
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
  local configured_url="${CODEQL_SERVER_URL:-}"
  local localhost_url="http://localhost:8080"
  local ip

  if [ -n "$configured_url" ]; then
    configured_url="${configured_url%/}"
    yellow "探测已配置的 CodeQL 服务器: $configured_url"
    if probe_server "$configured_url" "$connect_timeout" "$request_timeout"; then
      export CODEQL_SERVER_URL="$configured_url"
      green "CodeQL 服务器: $CODEQL_SERVER_URL"
      return 0
    fi
    red "已配置的 CodeQL 服务器不可达: $configured_url"
  fi

  if [ -z "$configured_url" ]; then
    yellow "尝试 $localhost_url"
    if probe_server "$localhost_url" "$connect_timeout" "$request_timeout"; then
      export CODEQL_SERVER_URL="$localhost_url"
      green "CodeQL 服务器: $CODEQL_SERVER_URL"
      return 0
    fi
  fi

  if [ ! -t 0 ]; then
    if [ -n "$configured_url" ]; then
      red "错误: 已配置的 CodeQL 服务不可达，非交互环境无法重试"
    else
      red "错误: CodeQL 服务不可达，非交互环境请设置 CODEQL_SERVER_URL"
    fi
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
      green "CodeQL 服务器: $CODEQL_SERVER_URL"
      return 0
    fi
    red "无法连接 $CODEQL_SERVER_URL，请重试"
  done
}

ensure_compile_database() {
  local database_target=""
  local stale_xmake=""
  local regenerate=0

  if [ -s "$COMPILE_DATABASE" ]; then
    database_target="$(readlink -e "$COMPILE_DATABASE" 2>/dev/null || true)"
  fi

  if [ -z "$database_target" ] || [ ! -s "$database_target" ]; then
    yellow "compile_commands.json 不存在，自动生成..."
    regenerate=1
  else
    stale_xmake=$(find "$PROJECT_ROOT" -type f -name 'xmake.lua' \
      ! -path "$PROJECT_ROOT/build/*" ! -path "$PROJECT_ROOT/.xmake/*" \
      -newer "$database_target" -print -quit)
    if [ -n "$stale_xmake" ]; then
      yellow "xmake.lua 已更新，重新生成 compile_commands.json..."
      regenerate=1
    else
      green "复用未过期的 compile_commands.json"
    fi
  fi

  if [ "$regenerate" -eq 1 ]; then
    (cd "$PROJECT_ROOT" && xmake project -k compile_commands)
  fi
  database_target="$(readlink -e "$COMPILE_DATABASE" 2>/dev/null || true)"
  if [ -z "$database_target" ] || [ ! -s "$database_target" ]; then
    red "错误: 未生成有效的 compile_commands.json"
    return 1
  fi
}

prepare_compile_database() {
  local output_file="$1"

  python3 - "$COMPILE_DATABASE" "$output_file" <<'PY'
import json
import os
import sys
from pathlib import PureWindowsPath

input_path, output_path = sys.argv[1:]
with open(input_path, encoding="utf-8") as source:
    data = json.load(source)

if not isinstance(data, list):
    raise SystemExit("错误: compile_commands.json 顶层必须是数组")

filtered = []
for entry in data:
    if not isinstance(entry, dict):
        continue
    file_path = str(entry.get("file", "")).replace("\\", "/")
    if not file_path:
        continue
    if os.path.isabs(file_path) or PureWindowsPath(file_path).is_absolute():
        continue
    normalized = dict(entry)
    normalized["directory"] = "."
    normalized["file"] = file_path
    filtered.append(normalized)

if not filtered:
    raise SystemExit("错误: 过滤后没有项目内编译条目")

with open(output_path, "w", encoding="utf-8") as target:
    json.dump(filtered, target, indent=2, ensure_ascii=False)
    target.write("\n")

print(f"过滤后保留 {len(filtered)}/{len(data)} 个编译条目")
PY
}

package_source() {
  local archive="$1"
  local path
  local source_paths=()
  local candidates=(
    tests xmake.lua src include core logger file-system memory-pool net db benchmark
  )

  for path in "${candidates[@]}"; do
    if [ -e "$PROJECT_ROOT/$path" ]; then
      source_paths+=("$path")
    fi
  done
  if [ ! -f "$PROJECT_ROOT/xmake.lua" ] || [ ! -d "$PROJECT_ROOT/tests" ]; then
    red "错误: CodeQL 打包缺少 xmake.lua 或 tests/"
    return 1
  fi

  (
    cd "$PROJECT_ROOT"
    tar czf "$archive" \
      --exclude='*.o' --exclude='*.obj' --exclude='*.exe' \
      --exclude='__pycache__' --exclude='.xmake' --exclude='build' \
      --exclude='benchmark/reports' --exclude='benchmark/reports/*' \
      "${source_paths[@]}"
  )
}

submit_analysis() {
  local archive="$1"
  local fixed_database="$2"
  local response_file="$3"
  local curl_exit
  local http_code
  local task_id
  local submit_timeout="${CODEQL_SUBMIT_TIMEOUT:-300}"

  if ! [[ "$submit_timeout" =~ ^[1-9][0-9]*$ ]]; then
    red "错误: CODEQL_SUBMIT_TIMEOUT 必须是正整数"
    return 1
  fi

  green "打包完成，发送到 CodeQL 服务器..."
  yellow "提交请求超时: ${submit_timeout} 秒；分析可能耗时 1-5 分钟..."

  set +e
  http_code=$(curl -sS --max-time "$submit_timeout" -X POST "$CODEQL_SERVER_URL/analyze" \
    -F "source=@$archive;type=application/gzip" \
    -F "compile_commands=@$fixed_database;type=application/json" \
    -o "$response_file" -w '%{http_code}')
  curl_exit=$?
  set -e

  if [ "$curl_exit" -ne 0 ] || ! [[ "$http_code" =~ ^2[0-9][0-9]$ ]]; then
    red "提交 CodeQL 分析失败（提交超时=${submit_timeout} 秒，curl=$curl_exit, HTTP=${http_code:-未知}）"
    if [ -s "$response_file" ]; then
      cat "$response_file"
    fi
    return 1
  fi

  if ! task_id=$(python3 - "$response_file" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    response = json.load(source)
task_id = response.get("task_id", "") if isinstance(response, dict) else ""
print(task_id)
PY
  ); then
    red "无法解析 CodeQL 提交响应"
    cat "$response_file"
    return 1
  fi

  if [ -z "$task_id" ] || ! [[ "$task_id" =~ ^[A-Za-z0-9._-]+$ ]]; then
    red "CodeQL 提交响应未包含有效任务 ID"
    cat "$response_file"
    return 1
  fi

  CODEQL_TASK_ID="$task_id"
  green "任务 ID: $CODEQL_TASK_ID"
}

classify_result_response() {
  local response_file="$1"

  python3 - "$response_file" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as source:
        payload = json.load(source)
except Exception:
    print("invalid")
    raise SystemExit(0)

sarif = payload.get("result") if isinstance(payload, dict) else None
if not isinstance(sarif, dict):
    sarif = payload
if isinstance(sarif, dict) and isinstance(sarif.get("runs"), list) and sarif["runs"]:
    print("ready")
elif isinstance(payload, dict) and str(payload.get("status", "")).lower() in {
    "failed", "error", "cancelled", "canceled"
}:
    print("failed")
else:
    print("pending")
PY
}

parse_sarif() {
  local input="$1"
  local parse_exit

  set +e
  python3 - "$input" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as source:
        payload = json.load(source)
    sarif = payload.get("result") if isinstance(payload, dict) else None
    if not isinstance(sarif, dict):
        sarif = payload
    runs = sarif.get("runs", []) if isinstance(sarif, dict) else []
    if not runs or not isinstance(runs[0], dict):
        raise ValueError("SARIF 缺少 runs[0]")

    run = runs[0]
    invocations = run.get("invocations", [])
    if not invocations or not isinstance(invocations[0], dict):
        raise ValueError("SARIF 缺少 invocations[0]")

    invocation = invocations[0]
    notifications = invocation.get("toolExecutionNotifications", [])
    for notification in notifications if isinstance(notifications, list) else []:
        if not isinstance(notification, dict):
            continue
        level = notification.get("level", "?")
        message = notification.get("message", {})
        text = message.get("text", "") if isinstance(message, dict) else str(message)
        print(f"工具通知[{level}]: {text}")

    if invocation.get("executionSuccessful") is not True:
        print("CodeQL 工具执行未成功")
        raise SystemExit(2)

    results = run.get("results", [])
    if not isinstance(results, list):
        raise ValueError("SARIF results 不是数组")

    critical = 0
    high = 0
    for result in results:
        if not isinstance(result, dict):
            continue
        rule_id = result.get("ruleId", "?")
        level = str(result.get("level", "")).lower()
        properties = result.get("properties", {})
        if not isinstance(properties, dict):
            properties = {}
        property_severity = str(properties.get("severity", "")).lower()
        severity = property_severity or level or "?"

        if level == "error" or property_severity in {"critical", "error"}:
            critical += 1
            gate_severity = "critical"
        elif level == "warning" or property_severity in {"high", "warning"}:
            high += 1
            gate_severity = "high"
        else:
            gate_severity = severity

        message = result.get("message", {})
        text = message.get("text", "") if isinstance(message, dict) else str(message)
        location_texts = []
        locations = result.get("locations", [])
        for location in locations if isinstance(locations, list) else []:
            physical = location.get("physicalLocation", {}) if isinstance(location, dict) else {}
            artifact = physical.get("artifactLocation", {}) if isinstance(physical, dict) else {}
            region = physical.get("region", {}) if isinstance(physical, dict) else {}
            uri = artifact.get("uri", "?") if isinstance(artifact, dict) else "?"
            line = region.get("startLine", "?") if isinstance(region, dict) else "?"
            location_texts.append(f"{uri}:{line}")
        location_summary = ", ".join(location_texts) if location_texts else "?:?"
        print(f"{gate_severity}:{rule_id} {location_summary} {text}")

    print(f"CodeQL 汇总: critical={critical}, high={high}")
    if critical or high:
        raise SystemExit(3)
except SystemExit:
    raise
except Exception as error:
    print(f"解析 CodeQL 响应失败: {error}")
    raise SystemExit(2)
PY
  parse_exit=$?
  set -e

  case "$parse_exit" in
    0) green "CodeQL 通过：0 critical + 0 high" ;;
    2) red "CodeQL 执行或结果解析失败"; return 1 ;;
    3) red "CodeQL 门禁未通过"; return 1 ;;
    *) red "CodeQL 解析器异常退出: $parse_exit"; return 1 ;;
  esac
}

poll_result() {
  local task_id="$1"
  local response_file="$2"
  local max_attempts="${CODEQL_POLL_ATTEMPTS:-120}"
  local interval="${CODEQL_POLL_INTERVAL:-5}"
  local attempt
  local curl_exit
  local http_code
  local state

  if ! [[ "$max_attempts" =~ ^[1-9][0-9]*$ ]]; then
    red "错误: CODEQL_POLL_ATTEMPTS 必须是正整数"
    return 1
  fi
  if ! [[ "$interval" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    red "错误: CODEQL_POLL_INTERVAL 必须是非负数"
    return 1
  fi

  yellow "轮询分析结果..."
  for ((attempt = 1; attempt <= max_attempts; attempt++)); do
    sleep "$interval"
    set +e
    http_code=$(curl -sS --max-time 10 \
      -o "$response_file" -w '%{http_code}' "$CODEQL_SERVER_URL/result/$task_id")
    curl_exit=$?
    set -e

    if [ "$curl_exit" -eq 0 ] && [[ "$http_code" =~ ^2[0-9][0-9]$ ]] && \
       [ -s "$response_file" ]; then
      state="$(classify_result_response "$response_file")"
      case "$state" in
        ready)
          green "结果就绪"
          parse_sarif "$response_file"
          return $?
          ;;
        failed)
          red "CodeQL 服务报告任务失败"
          cat "$response_file"
          return 1
          ;;
        invalid)
          red "CodeQL 服务返回了无效 JSON"
          cat "$response_file"
          return 1
          ;;
      esac
    fi
  done

  red "轮询超时（尝试 ${max_attempts} 次，间隔 ${interval} 秒）"
  if [ -s "$response_file" ]; then
    cat "$response_file"
  fi
  return 1
}

cmd_run() (
  blue "=== CodeQL ==="
  check_required_tools
  discover_server

  local tmpdir
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' EXIT

  cd "$PROJECT_ROOT"
  ensure_compile_database
  prepare_compile_database "$tmpdir/compile_commands_fixed.json"
  package_source "$tmpdir/source.tar.gz"
  submit_analysis "$tmpdir/source.tar.gz" "$tmpdir/compile_commands_fixed.json" \
    "$tmpdir/post_response.json"
  poll_result "$CODEQL_TASK_ID" "$tmpdir/result_response.json"
)

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
