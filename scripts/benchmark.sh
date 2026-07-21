#!/usr/bin/env bash
# ============================================================
# benchmark.sh - 微基准、模块 QPS 与端到端 HTTP RPS 测试
# ============================================================
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"
cd "$PROJECT_ROOT"

export LD_LIBRARY_PATH="$PROJECT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

REPORT_DIR="$PROJECT_ROOT/benchmark/reports"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BENCH_HOSTNAME=$(hostname 2>/dev/null || echo "unknown")
CPU_MODEL=$(awk -F ': ' '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null || echo "unknown")
CPU_CORES=$(nproc 2>/dev/null || echo "unknown")
MEMORY_KB=$(awk '/MemTotal/{print $2; exit}' /proc/meminfo 2>/dev/null || echo "unknown")
if git diff --quiet --ignore-submodules HEAD -- 2>/dev/null && \
   [ -z "$(git ls-files --others --exclude-standard 2>/dev/null)" ]; then
  GIT_DIRTY=false
else
  GIT_DIRTY=true
fi
mkdir -p "$REPORT_DIR"

require_tools() {
  local missing=0
  local tool
  for tool in "$@"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      red "错误: 缺少命令 $tool"
      missing=1
    fi
  done
  return "$missing"
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_nonnegative_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

read_default_rps_port() {
  local env_file="$PROJECT_ROOT/.env"
  local line value="" found=0

  # 只解析端口字段，绝不 source .env，避免执行部署密钥或其他任意内容。
  if [ -L "$env_file" ]; then
    red "错误: $env_file 不允许是符号链接，拒绝读取 RPS 默认端口" >&2
    return 1
  fi
  if [ ! -e "$env_file" ]; then
    printf '%s\n' "18080"
    return 0
  fi
  if [ ! -f "$env_file" ]; then
    red "错误: $env_file 不是普通文件，拒绝读取 RPS 默认端口" >&2
    return 1
  fi

  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      HPS_HTTP_PORT=*)
        value="${line#HPS_HTTP_PORT=}"
        found=1
        ;;
    esac
  done < "$env_file"

  if [ "$found" -eq 0 ]; then
    printf '%s\n' "18080"
    return 0
  fi
  if [ "${#value}" -ge 2 ]; then
    if { [ "${value:0:1}" = '"' ] && [ "${value: -1}" = '"' ]; } || \
       { [ "${value:0:1}" = "'" ] && [ "${value: -1}" = "'" ]; }; then
      value="${value:1:${#value}-2}"
    fi
  fi
  if ! is_positive_integer "$value" || [ "$value" -gt 65535 ]; then
    red "错误: .env 中 HPS_HTTP_PORT 必须是 1 到 65535 的整数" >&2
    return 1
  fi
  printf '%s\n' "$value"
}

parse_integer_list() {
  local value="$1"
  local output_name="$2"
  local -n output_ref="$output_name"
  read -r -a output_ref <<< "$value"
  if [ "${#output_ref[@]}" -eq 0 ]; then
    red "错误: 并发列表不能为空"
    return 1
  fi
  local item
  for item in "${output_ref[@]}"; do
    if ! is_positive_integer "$item"; then
      red "错误: 非法正整数: $item"
      return 1
    fi
  done
}

write_report_header() {
  local title="$1"
  local profile="${2:--}"
  cat <<EOF
==========================================
  $title
  时间: $TIMESTAMP
  Git:  $GIT_HASH (dirty=$GIT_DIRTY)
  主机: $BENCH_HOSTNAME
  CPU:  $CPU_MODEL
  核心: $CPU_CORES
  Profile: $profile
==========================================

EOF
}

cmd_build() {
  blue "=== 编译 benchmark（Release 模式）==="
  local mode="release"
  if [ "${1:-}" = "--debug" ]; then
    mode="debug"
    yellow "警告: debug 模式数据不用于性能基线"
  fi
  xmake f -m "$mode" -y
  xmake -j"$(nproc)"
  green "编译完成（${mode}）"
}

cmd_micro() {
  require_tools xmake python3 find sort
  cmd_build "$@"

  local report_file="$REPORT_DIR/micro_${TIMESTAMP}.txt"
  local json_file="$REPORT_DIR/micro_${TIMESTAMP}.json"
  local raw_dir="$REPORT_DIR/micro_${TIMESTAMP}_raw"
  local result_file
  result_file=$(mktemp "/tmp/hps_micro_${TIMESTAMP}.XXXXXX")
  mkdir -p "$raw_dir"
  printf 'target\tstatus\texit_code\traw_file\n' > "$result_file"
  write_report_header "微基准测试报告" "${BENCH_PROFILE:-default}" > "$report_file"

  local -a bench_bins=()
  mapfile -t bench_bins < <(find "$PROJECT_ROOT/bin" -maxdepth 1 -type f -name 'bench_*' -printf '%p\n' | sort)
  if [ "${#bench_bins[@]}" -eq 0 ]; then
    rm -f "$result_file"
    red "未找到微基准二进制（bin/bench_*）"
    return 1
  fi

  local -a bench_flags=()
  read -r -a bench_flags <<< "${BENCH_FLAGS:---benchmark_min_time=0.1s}"
  local failures=0
  local bin name raw_file raw_rel rc status
  for bin in "${bench_bins[@]}"; do
    name=$(basename "$bin")
    raw_file="$raw_dir/${name}_${TIMESTAMP}.txt"
    raw_rel="${raw_file#"$PROJECT_ROOT/"}"
    yellow "运行微基准: $name"
    set +e
    "$bin" "${bench_flags[@]}" > "$raw_file" 2>&1
    rc=$?
    set -e
    cat "$raw_file"
    if [ "$rc" -eq 0 ]; then
      status="passed"
    else
      status="failed"
      failures=$((failures + 1))
    fi
    {
      printf '\n--- %s (status=%s, exit=%s) ---\n' "$name" "$status" "$rc"
      cat "$raw_file"
    } >> "$report_file"
    printf '%s\t%s\t%s\t%s\n' "$name" "$status" "$rc" "$raw_rel" >> "$result_file"
  done

  python3 - "$result_file" "$json_file" "$TIMESTAMP" "$GIT_HASH" "$GIT_DIRTY" \
    "$BENCH_HOSTNAME" "$CPU_MODEL" "$CPU_CORES" "$MEMORY_KB" <<'PY'
import csv
import json
import sys

tsv, output, timestamp, git_hash, dirty, hostname, cpu, cores, memory = sys.argv[1:]
with open(tsv, encoding="utf-8") as stream:
    rows = list(csv.DictReader(stream, delimiter="\t"))
for row in rows:
    row["exit_code"] = int(row["exit_code"])
data = {
    "type": "micro",
    "environment": {
        "timestamp": timestamp,
        "git_hash": git_hash,
        "git_dirty": dirty == "true",
        "hostname": hostname,
        "cpu_model": cpu,
        "cpu_cores": cores,
        "memory_kb": memory,
    },
    "summary": {
        "total": len(rows),
        "passed": sum(row["status"] == "passed" for row in rows),
        "failed": sum(row["status"] != "passed" for row in rows),
    },
    "results": rows,
}
with open(output, "w", encoding="utf-8") as stream:
    json.dump(data, stream, ensure_ascii=False, indent=2)
    stream.write("\n")
PY
  rm -f "$result_file"
  green "文本报告: $report_file"
  green "JSON 报告: $json_file"
  green "原始输出: $raw_dir"
  [ "$failures" -eq 0 ]
}

discover_qps_targets() {
  QPS_EXPECTED_TARGETS=()
  local source
  while IFS= read -r source; do
    QPS_EXPECTED_TARGETS+=("$(basename "${source%.cpp}")")
  done < <(find "$PROJECT_ROOT/benchmark" -maxdepth 1 -type f -name 'qps_*.cpp' -printf '%p\n' | sort)
}

validate_qps_artifacts() {
  QPS_MISSING_TARGETS=()
  QPS_STALE_TARGETS=()
  local -A expected=()
  local target artifact
  for target in "${QPS_EXPECTED_TARGETS[@]}"; do
    expected["$target"]=1
    if [ ! -x "$PROJECT_ROOT/bin/$target" ]; then
      QPS_MISSING_TARGETS+=("$target")
    fi
  done
  while IFS= read -r artifact; do
    target=$(basename "$artifact")
    if [ -z "${expected[$target]:-}" ]; then
      QPS_STALE_TARGETS+=("$target")
    fi
  done < <(find "$PROJECT_ROOT/bin" -maxdepth 1 -type f -name 'qps_*' -printf '%p\n' | sort)
  [ "${#QPS_MISSING_TARGETS[@]}" -eq 0 ] && [ "${#QPS_STALE_TARGETS[@]}" -eq 0 ]
}

write_qps_json() {
  local result_file="$1"
  local output_file="$2"
  local profile="$3"
  python3 - "$result_file" "$output_file" "$profile" "$TIMESTAMP" "$GIT_HASH" "$GIT_DIRTY" \
    "$BENCH_HOSTNAME" "$CPU_MODEL" "$CPU_CORES" "$MEMORY_KB" <<'PY'
import csv
import json
import sys

tsv, output, profile, timestamp, git_hash, dirty, hostname, cpu, cores, memory = sys.argv[1:]
with open(tsv, encoding="utf-8") as stream:
    rows = list(csv.DictReader(stream, delimiter="\t"))
for row in rows:
    row["exit_code"] = int(row["exit_code"])
    row["elapsed_seconds"] = int(row["elapsed_seconds"])
data = {
    "type": "qps",
    "profile": profile,
    "environment": {
        "timestamp": timestamp,
        "git_hash": git_hash,
        "git_dirty": dirty == "true",
        "hostname": hostname,
        "cpu_model": cpu,
        "cpu_cores": cores,
        "memory_kb": memory,
    },
    "summary": {
        "expected_targets": len(rows),
        "passed": sum(row["status"] == "passed" for row in rows),
        "failed": sum(row["status"] == "failed" for row in rows),
        "timed_out": sum(row["status"] == "timeout" for row in rows),
    },
    "results": rows,
}
with open(output, "w", encoding="utf-8") as stream:
    json.dump(data, stream, ensure_ascii=False, indent=2)
    stream.write("\n")
PY
}

cmd_qps() {
  require_tools xmake timeout python3 find sort awk
  local profile="${QPS_PROFILE:-smoke}"
  if [ "${1:-}" = "smoke" ] || [ "${1:-}" = "full" ]; then
    profile="$1"
    shift
  fi
  case "$profile" in
    smoke|full) ;;
    *) red "错误: QPS_PROFILE 仅支持 smoke 或 full"; return 2 ;;
  esac

  cmd_build "$@"
  discover_qps_targets
  if [ "${#QPS_EXPECTED_TARGETS[@]}" -eq 0 ]; then
    red "错误: benchmark/qps_*.cpp 为空"
    return 1
  fi
  if ! validate_qps_artifacts; then
    [ "${#QPS_MISSING_TARGETS[@]}" -gt 0 ] && red "缺失 QPS 二进制: ${QPS_MISSING_TARGETS[*]}"
    [ "${#QPS_STALE_TARGETS[@]}" -gt 0 ] && red "发现陈旧 QPS 二进制: ${QPS_STALE_TARGETS[*]}"
    red "拒绝运行：QPS 源文件与 bin/ 目标不一致"
    return 1
  fi

  local default_timeout=900
  [ "$profile" = "smoke" ] && default_timeout=120
  local timeout_seconds="${QPS_TIMEOUT_SECONDS:-$default_timeout}"
  if ! is_positive_integer "$timeout_seconds"; then
    red "错误: QPS_TIMEOUT_SECONDS 必须为正整数"
    return 2
  fi

  local report_file="$REPORT_DIR/qps_${TIMESTAMP}.txt"
  local json_file="$REPORT_DIR/qps_${TIMESTAMP}.json"
  local raw_dir="$REPORT_DIR/qps_${TIMESTAMP}_raw"
  local result_file
  result_file=$(mktemp "/tmp/hps_qps_${TIMESTAMP}.XXXXXX")
  mkdir -p "$raw_dir"
  printf 'target\tstatus\texit_code\telapsed_seconds\traw_file\n' > "$result_file"
  write_report_header "QPS + 压力测试报告" "$profile" > "$report_file"
  printf '目标数: %s\n单目标超时: %ss\n\n' "${#QPS_EXPECTED_TARGETS[@]}" "$timeout_seconds" >> "$report_file"

  export QPS_PROFILE="$profile"
  local failures=0
  local target bin raw_file raw_rel rc status started elapsed
  for target in "${QPS_EXPECTED_TARGETS[@]}"; do
    bin="$PROJECT_ROOT/bin/$target"
    raw_file="$raw_dir/${target}_${TIMESTAMP}.txt"
    raw_rel="${raw_file#"$PROJECT_ROOT/"}"
    yellow "运行 QPS: $target (profile=$profile, timeout=${timeout_seconds}s)"
    started=$(date +%s)
    set +e
    timeout --signal=TERM --kill-after=5s "${timeout_seconds}s" "$bin" > "$raw_file" 2>&1
    rc=$?
    set -e
    elapsed=$(( $(date +%s) - started ))
    cat "$raw_file"
    if [ "$rc" -eq 0 ]; then
      status="passed"
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      status="timeout"
      failures=$((failures + 1))
    else
      status="failed"
      failures=$((failures + 1))
    fi
    {
      printf '\n--- %s (status=%s, exit=%s, elapsed=%ss) ---\n' "$target" "$status" "$rc" "$elapsed"
      cat "$raw_file"
    } >> "$report_file"
    printf '%s\t%s\t%s\t%s\t%s\n' "$target" "$status" "$rc" "$elapsed" "$raw_rel" >> "$result_file"
  done

  {
    echo
    echo "=== QPS 目标汇总 ==="
    printf '%-32s %-10s %-8s %-10s\n' "目标" "状态" "退出码" "耗时(s)"
    awk -F '\t' 'NR > 1 {printf "%-32s %-10s %-8s %-10s\n", $1, $2, $3, $4}' "$result_file"
  } | tee -a "$report_file"
  write_qps_json "$result_file" "$json_file" "$profile"
  rm -f "$result_file"

  green "文本报告: $report_file"
  green "JSON 报告: $json_file"
  green "原始输出: $raw_dir"
  if [ "$failures" -ne 0 ]; then
    red "QPS 失败目标数: $failures"
    return 1
  fi
  green "QPS 全部 ${#QPS_EXPECTED_TARGETS[@]} 个目标通过"
}

create_wrk_lua() {
  local output_file="$1"
  local method="$2"
  local body_file="$3"
  local content_type="$4"
  local token="$5"
  local range_header="$6"
  local expected_codes="$7"
  local disposition="$8"
  local unique_body="$9"
  local request_nonce="${10}"
  local request_delay_ms="${11}"
  local expected_lua=""
  local -a codes=()
  local code
  IFS=',' read -r -a codes <<< "$expected_codes"
  for code in "${codes[@]}"; do
    expected_lua+="[$code]=true,"
  done

  cat > "$output_file" <<LUA
local expected = {${expected_lua}}
local payload = ""
local body_file = [==[${body_file}]==]
local unique_body = [==[${unique_body}]==] == "1"
local request_nonce = [==[${request_nonce}]==]
local request_delay_ms = tonumber([==[${request_delay_ms}]==]) or 0
local threads = {}
local next_thread_id = 0

if body_file ~= "" then
  local stream = assert(io.open(body_file, "rb"))
  payload = stream:read("*all")
  stream:close()
end

wrk.method = [==[${method}]==]
wrk.body = payload
if [==[${content_type}]==] ~= "" then
  wrk.headers["Content-Type"] = [==[${content_type}]==]
end
if [==[${token}]==] ~= "" then
  wrk.headers["Authorization"] = "Bearer " .. [==[${token}]==]
end
if [==[${range_header}]==] ~= "" then
  wrk.headers["Range"] = [==[${range_header}]==]
end
if [==[${disposition}]==] ~= "" then
  wrk.headers["Content-Disposition"] = [==[${disposition}]==]
end
if request_delay_ms > 0 then
  delay = function()
    return request_delay_ms
  end
end

setup = function(thread)
  next_thread_id = next_thread_id + 1
  thread:set("hps_thread_id", next_thread_id)
  table.insert(threads, thread)
end

init = function()
  hps_request_counter = 0
  hps_status_total = 0
  hps_status_2xx = 0
  hps_status_non_2xx = 0
  hps_status_unexpected = 0
  for status = 100, 599 do
    _G["hps_status_" .. tostring(status)] = 0
  end
end

request = function()
  if not unique_body then
    return wrk.format()
  end

  hps_request_counter = hps_request_counter + 1
  local marker = request_nonce .. ":" .. tostring(hps_thread_id) .. ":" .. tostring(hps_request_counter) .. ":"
  assert(#marker <= #payload, "unique request marker exceeds upload payload")
  local request_body = marker .. string.sub(payload, #marker + 1)
  return wrk.format(wrk.method, wrk.path, wrk.headers, request_body)
end

response = function(status)
  hps_status_total = hps_status_total + 1
  if status >= 200 and status < 300 then
    hps_status_2xx = hps_status_2xx + 1
  else
    hps_status_non_2xx = hps_status_non_2xx + 1
  end
  if not expected[status] then
    hps_status_unexpected = hps_status_unexpected + 1
  end
  local status_name = "hps_status_" .. tostring(status)
  _G[status_name] = (_G[status_name] or 0) + 1
end

done = function(summary)
  local total = 0
  local success = 0
  local non_2xx = 0
  local unexpected = 0
  local status_counts = {}
  for _, thread in ipairs(threads) do
    total = total + (thread:get("hps_status_total") or 0)
    success = success + (thread:get("hps_status_2xx") or 0)
    non_2xx = non_2xx + (thread:get("hps_status_non_2xx") or 0)
    unexpected = unexpected + (thread:get("hps_status_unexpected") or 0)
    for status = 100, 599 do
      local count = thread:get("hps_status_" .. tostring(status)) or 0
      if count > 0 then
        status_counts[status] = (status_counts[status] or 0) + count
      end
    end
  end

  local keys = {}
  for status in pairs(status_counts) do
    table.insert(keys, status)
  end
  table.sort(keys)
  local parts = {}
  for _, status in ipairs(keys) do
    table.insert(parts, tostring(status) .. "=" .. tostring(status_counts[status]))
  end
  local errors = summary.errors or {}
  io.write("HPS_WRK_REQUESTS " .. tostring(summary.requests or 0) .. "\n")
  io.write("HPS_WRK_STATUS_ERRORS " .. tostring(errors.status or 0) .. "\n")
  io.write("HPS_SOCKET_CONNECT " .. tostring(errors.connect or 0) .. "\n")
  io.write("HPS_SOCKET_READ " .. tostring(errors.read or 0) .. "\n")
  io.write("HPS_SOCKET_WRITE " .. tostring(errors.write or 0) .. "\n")
  io.write("HPS_SOCKET_TIMEOUT " .. tostring(errors.timeout or 0) .. "\n")
  io.write("HPS_STATUS_TOTAL " .. tostring(total) .. "\n")
  io.write("HPS_STATUS_2XX " .. tostring(success) .. "\n")
  io.write("HPS_STATUS_NON_2XX " .. tostring(non_2xx) .. "\n")
  io.write("HPS_STATUS_UNEXPECTED " .. tostring(unexpected) .. "\n")
  io.write("HPS_STATUS_CODES " .. table.concat(parts, ",") .. "\n")
end
LUA
}

raw_metric() {
  local pattern="$1"
  local field="$2"
  local file="$3"
  awk -v pattern="$pattern" -v field="$field" '$0 ~ pattern {print $field; exit}' "$file"
}

run_rps_cell() {
  local index="$1"
  local scenario="$2"
  local method="$3"
  local path="$4"
  local payload="$5"
  local body_file="$6"
  local content_type="$7"
  local token="$8"
  local range_header="$9"
  local expected_codes="${10}"
  local disposition="${11}"
  local concurrency="${12}"
  local duration="${13}"
  local repeat="${14}"
  local strict_errors="${15}"
  local unique_body="${16}"
  local request_nonce="${17}"
  local request_delay_ms="${18}"

  local cell_id
  cell_id=$(printf '%03d_%s_c%s_r%s' "$index" "$scenario" "$concurrency" "$repeat")
  local lua_file="$RPS_TMP_DIR/${cell_id}.lua"
  local raw_file="$RPS_RAW_DIR/${cell_id}_${TIMESTAMP}.txt"
  local raw_rel="${raw_file#"$PROJECT_ROOT/"}"
  create_wrk_lua "$lua_file" "$method" "$body_file" "$content_type" "$token" \
    "$range_header" "$expected_codes" "$disposition" "$unique_body" "$request_nonce" "$request_delay_ms"

  local threads="$concurrency"
  if [ "$threads" -gt "$CPU_CORES" ] 2>/dev/null; then
    threads="$CPU_CORES"
  fi
  local cell_timeout=$((duration + RPS_CELL_TIMEOUT_GRACE_SECONDS))
  local url="${RPS_BASE_URL}${path}"
  yellow "RPS: $scenario $method $path 并发=$concurrency 时长=${duration}s 重复=$repeat"
  set +e
  timeout --signal=TERM --kill-after=5s "${cell_timeout}s" \
    wrk -t"$threads" -c"$concurrency" -d"${duration}s" \
      --timeout "${RPS_REQUEST_TIMEOUT_SECONDS}s" --latency -s "$lua_file" "$url" \
      > "$raw_file" 2>&1
  local rc=$?
  set -e
  cat "$raw_file"

  local rps avg_latency p50 p90 p99 wrk_requests wrk_status_errors
  local status_total status_2xx non_2xx unexpected status_codes
  local socket_connect socket_read socket_write socket_timeout
  rps=$(raw_metric 'Requests/sec' 2 "$raw_file" || true)
  avg_latency=$(raw_metric 'Latency' 2 "$raw_file" || true)
  p50=$(raw_metric '^[[:space:]]+50%' 2 "$raw_file" || true)
  p90=$(raw_metric '^[[:space:]]+90%' 2 "$raw_file" || true)
  p99=$(raw_metric '^[[:space:]]+99%' 2 "$raw_file" || true)
  wrk_requests=$(raw_metric '^HPS_WRK_REQUESTS ' 2 "$raw_file" || true)
  wrk_status_errors=$(raw_metric '^HPS_WRK_STATUS_ERRORS ' 2 "$raw_file" || true)
  status_total=$(raw_metric '^HPS_STATUS_TOTAL ' 2 "$raw_file" || true)
  status_2xx=$(raw_metric '^HPS_STATUS_2XX ' 2 "$raw_file" || true)
  non_2xx=$(raw_metric '^HPS_STATUS_NON_2XX ' 2 "$raw_file" || true)
  unexpected=$(raw_metric '^HPS_STATUS_UNEXPECTED ' 2 "$raw_file" || true)
  status_codes=$(raw_metric '^HPS_STATUS_CODES ' 2 "$raw_file" || true)
  socket_connect=$(raw_metric '^HPS_SOCKET_CONNECT ' 2 "$raw_file" || true)
  socket_read=$(raw_metric '^HPS_SOCKET_READ ' 2 "$raw_file" || true)
  socket_write=$(raw_metric '^HPS_SOCKET_WRITE ' 2 "$raw_file" || true)
  socket_timeout=$(raw_metric '^HPS_SOCKET_TIMEOUT ' 2 "$raw_file" || true)
  status_codes=${status_codes:--}

  local metric metric_valid=1
  for metric in "$wrk_requests" "$wrk_status_errors" "$status_total" "$status_2xx" \
    "$non_2xx" "$unexpected" "$socket_connect" "$socket_read" "$socket_write" "$socket_timeout"; do
    if ! is_nonnegative_integer "$metric"; then
      metric_valid=0
    fi
  done

  local result="passed"
  local error_reason="-"
  if [ "$rc" -ne 0 ]; then
    result="failed"
    error_reason="wrk_exit_${rc}"
  elif [ -z "$rps" ] || [ "$metric_valid" -ne 1 ] || \
       ! is_positive_integer "$wrk_requests" || ! is_positive_integer "$status_total"; then
    result="failed"
    error_reason="missing_metrics"
  elif [ "$status_total" -ne "$wrk_requests" ] || \
       [ $((status_2xx + non_2xx)) -ne "$status_total" ] || \
       [ "$non_2xx" -ne "$wrk_status_errors" ]; then
    result="failed"
    error_reason="status_count_mismatch"
  elif [ "$non_2xx" -gt 0 ] || [ "$unexpected" -gt 0 ] || \
       [ "$socket_connect" -gt 0 ] || [ "$socket_read" -gt 0 ] || \
       [ "$socket_write" -gt 0 ] || [ "$socket_timeout" -gt 0 ]; then
    if [ "$strict_errors" -eq 1 ]; then
      result="failed"
      error_reason="http_or_socket_errors"
    else
      result="overloaded"
      error_reason="overload_errors_observed"
    fi
  fi
  if [ "$result" = "failed" ]; then
    RPS_FAILED_CELLS=$((RPS_FAILED_CELLS + 1))
  elif [ "$result" = "overloaded" ]; then
    RPS_OVERLOADED_CELLS=$((RPS_OVERLOADED_CELLS + 1))
  fi
  RPS_COMPLETED_CELLS=$((RPS_COMPLETED_CELLS + 1))

  printf '结果: %s RPS=%s status=%s non-2xx=%s unexpected=%s socket=%s/%s/%s/%s\n' \
    "$result" "${rps:-N/A}" "$status_codes" "$non_2xx" "$unexpected" \
    "$socket_connect" "$socket_read" "$socket_write" "$socket_timeout"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$index" "$scenario" "$method" "$path" "$payload" "$concurrency" "$duration" "$repeat" \
    "$rc" "$result" "$error_reason" "${wrk_requests:-0}" "${wrk_status_errors:-0}" \
    "${status_total:-0}" "${status_2xx:-0}" "${non_2xx:-0}" "${unexpected:-0}" \
    "${socket_connect:-0}" "${socket_read:-0}" "${socket_write:-0}" "${socket_timeout:-0}" "${rps:-0}" \
    "${avg_latency:-N/A}" "${p50:-N/A}" "${p90:-N/A}" "${p99:-N/A}" "$status_codes" "$raw_rel" \
    >> "$RPS_RESULTS_FILE"
}

write_rps_reports() {
  local profile="$1"
  local strict_errors="$2"
  local preflight_status="$3"
  local preflight_error="$4"
  python3 - "$RPS_RESULTS_FILE" "$RPS_JSON_FILE" "$profile" "$RPS_BASE_URL" \
    "$RPS_EXPECTED_CELLS" "$strict_errors" "$preflight_status" "$preflight_error" \
    "$RPS_UPLOAD_DURATION_SECONDS" "$RPS_UPLOAD_DELAY_MILLISECONDS" \
    "$TIMESTAMP" "$GIT_HASH" "$GIT_DIRTY" "$BENCH_HOSTNAME" "$CPU_MODEL" "$CPU_CORES" "$MEMORY_KB" <<'PY'
import csv
import json
import sys

(
    tsv,
    output,
    profile,
    base_url,
    expected_cells,
    strict_errors,
    preflight_status,
    preflight_error,
    upload_duration,
    upload_delay,
    timestamp,
    git_hash,
    dirty,
    hostname,
    cpu,
    cores,
    memory,
) = sys.argv[1:]
with open(tsv, encoding="utf-8") as stream:
    rows = list(csv.DictReader(stream, delimiter="\t"))
integer_fields = {
    "index", "concurrency", "duration_seconds", "repeat", "exit_code",
    "wrk_requests", "wrk_status_errors",
    "status_total", "status_2xx", "non_2xx", "unexpected_status",
    "socket_connect", "socket_read", "socket_write", "socket_timeout",
}
for row in rows:
    for field in integer_fields:
        row[field] = int(row[field])
    try:
        row["rps"] = float(row["rps"])
    except ValueError:
        row["rps"] = 0.0
socket_errors = sum(
    row["socket_connect"] + row["socket_read"] + row["socket_write"] + row["socket_timeout"]
    for row in rows
)
failed = sum(row["result"] == "failed" for row in rows)
summary = {
    "expected_cells": int(expected_cells),
    "completed_cells": len(rows),
    "passed_cells": sum(row["result"] == "passed" for row in rows),
    "overloaded_cells": sum(row["result"] == "overloaded" for row in rows),
    "failed_cells": failed,
    "non_2xx": sum(row["non_2xx"] for row in rows),
    "unexpected_status": sum(row["unexpected_status"] for row in rows),
    "socket_errors": socket_errors,
    "preflight_status": preflight_status,
}
summary["result"] = "passed" if (
    preflight_status == "passed"
    and len(rows) == int(expected_cells)
    and failed == 0
) else "failed"
data = {
    "type": "rps",
    "profile": profile,
    "base_url": base_url,
    "strict_errors": strict_errors == "1",
    "upload_guard": {
        "duration_seconds": int(upload_duration),
        "request_delay_milliseconds": int(upload_delay),
    },
    "environment": {
        "timestamp": timestamp,
        "git_hash": git_hash,
        "git_dirty": dirty == "true",
        "hostname": hostname,
        "cpu_model": cpu,
        "cpu_cores": cores,
        "memory_kb": memory,
    },
    "preflight": {"status": preflight_status, "error": preflight_error},
    "summary": summary,
    "results": rows,
}
with open(output, "w", encoding="utf-8") as stream:
    json.dump(data, stream, ensure_ascii=False, indent=2)
    stream.write("\n")
PY

  {
    write_report_header "HTTP RPS 压力测试报告" "$profile"
    python3 - "$RPS_JSON_FILE" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
summary = data["summary"]
print(f"入口: {data['base_url']}")
print(
    "上传保护: "
    f"单元时长={data['upload_guard']['duration_seconds']}s "
    f"请求间隔={data['upload_guard']['request_delay_milliseconds']}ms"
)
print(f"预检: {summary['preflight_status']}")
print(f"矩阵: {summary['completed_cells']}/{summary['expected_cells']}")
print(f"通过: {summary['passed_cells']}  过载观测: {summary['overloaded_cells']}  失败: {summary['failed_cells']}")
print(f"non-2xx: {summary['non_2xx']}  unexpected: {summary['unexpected_status']}  socket errors: {summary['socket_errors']}")
print(f"结论: {summary['result']}")
print()
print(f"{'场景':20s} {'方法':6s} {'载荷':8s} {'并发':>6s} {'RPS':>12s} {'P50':>10s} {'P90':>10s} {'P99':>10s} {'non2xx':>8s} {'socket':>8s} {'结果':>12s}")
print("=" * 122)
for row in data["results"]:
    socket_errors = row["socket_connect"] + row["socket_read"] + row["socket_write"] + row["socket_timeout"]
    print(
        f"{row['scenario']:20.20s} {row['method']:6s} {row['payload']:8s} "
        f"{row['concurrency']:6d} {row['rps']:12.1f} {row['p50']:>10s} {row['p90']:>10s} "
        f"{row['p99']:>10s} {row['non_2xx']:8d} {socket_errors:8d} {row['result']:>12s}"
    )
PY
  } > "$RPS_TEXT_FILE"
}

rps_preflight_failure() {
  local message="$1"
  red "$message"
  write_rps_reports "$RPS_ACTIVE_PROFILE" "$RPS_STRICT_ERRORS" "failed" "$message"
  green "文本报告: $RPS_TEXT_FILE"
  green "JSON 报告: $RPS_JSON_FILE"
  green "原始输出: $RPS_RAW_DIR"
  cleanup_rps_tmp
  trap - EXIT
  return 1
}

cleanup_rps_tmp() {
  if [[ -n "${RPS_TMP_DIR:-}" && -d "${RPS_TMP_DIR:-}" && "${RPS_TMP_DIR:-}" == /tmp/hps_rps_* ]]; then
    rm -rf -- "$RPS_TMP_DIR"
  fi
}

cmd_rps() {
  require_tools wrk curl python3 timeout dd awk
  local profile="${RPS_PROFILE:-smoke}"
  if [ "${1:-}" = "smoke" ] || [ "${1:-}" = "full" ] || [ "${1:-}" = "overload" ]; then
    profile="$1"
    shift
  fi
  if [ "$#" -ne 0 ]; then
    red "错误: rps 不接受额外参数: $*"
    return 2
  fi

  local default_duration default_read default_transfer default_upload
  local default_upload_duration default_upload_delay strict_errors
  case "$profile" in
    smoke)
      default_duration=5
      default_read="1 10"
      default_transfer="1 5"
      default_upload="1"
      default_upload_duration=2
      default_upload_delay=250
      strict_errors=1
      ;;
    full)
      default_duration=20
      default_read="1 10 50 100 500 1000"
      default_transfer="1 10 50 100"
      default_upload="1 2"
      default_upload_duration=5
      default_upload_delay=250
      strict_errors=1
      ;;
    overload)
      default_duration=20
      default_read="2000 5000 10000"
      default_transfer="100 500 1000"
      default_upload="2 5"
      default_upload_duration=5
      default_upload_delay=250
      strict_errors=0
      ;;
    *)
      red "错误: RPS_PROFILE 仅支持 smoke、full 或 overload"
      return 2
      ;;
  esac

  local duration="${RPS_DURATION_SECONDS:-$default_duration}"
  local repeats="${RPS_REPEATS:-1}"
  RPS_REQUEST_TIMEOUT_SECONDS="${RPS_REQUEST_TIMEOUT_SECONDS:-5}"
  RPS_CELL_TIMEOUT_GRACE_SECONDS="${RPS_CELL_TIMEOUT_GRACE_SECONDS:-30}"
  RPS_UPLOAD_DURATION_SECONDS="${RPS_UPLOAD_DURATION_SECONDS:-$default_upload_duration}"
  RPS_UPLOAD_DELAY_MILLISECONDS="${RPS_UPLOAD_DELAY_MILLISECONDS:-$default_upload_delay}"
  if ! is_positive_integer "$duration" || ! is_positive_integer "$repeats" || \
     ! is_positive_integer "$RPS_REQUEST_TIMEOUT_SECONDS" || \
     ! is_positive_integer "$RPS_CELL_TIMEOUT_GRACE_SECONDS" || \
     ! is_positive_integer "$RPS_UPLOAD_DURATION_SECONDS" || \
     ! is_nonnegative_integer "$RPS_UPLOAD_DELAY_MILLISECONDS"; then
    red "错误: RPS 时长、重复次数和超时必须为正整数"
    return 2
  fi

  local -a read_concurrency=()
  local -a transfer_concurrency=()
  local -a upload_concurrency=()
  parse_integer_list "${RPS_READ_CONCURRENCY:-$default_read}" read_concurrency
  parse_integer_list "${RPS_TRANSFER_CONCURRENCY:-$default_transfer}" transfer_concurrency
  parse_integer_list "${RPS_UPLOAD_CONCURRENCY:-$default_upload}" upload_concurrency

  local default_port
  if [ -n "${RPS_BASE_URL:-}" ]; then
    RPS_BASE_URL="$RPS_BASE_URL"
  else
    if ! default_port="$(read_default_rps_port)"; then
      return 2
    fi
    if [ "$default_port" = "8080" ]; then
      red "错误: 未显式设置 RPS_BASE_URL 时，拒绝使用 .env 中 HPS_HTTP_PORT=8080；8080 为 CodeQL 保留端口。请将 .env 改为建议的 nginx 公共端口 18080，或显式设置 RPS_BASE_URL。"
      return 2
    fi
    RPS_BASE_URL="http://127.0.0.1:${default_port}"
  fi
  RPS_BASE_URL="${RPS_BASE_URL%/}"
  if [[ ! "$RPS_BASE_URL" =~ ^https?://[^/?#[:space:]]+$ ]]; then
    red "错误: RPS_BASE_URL 必须是无路径、查询或片段的 http(s) 入口"
    return 2
  fi

  RPS_ACTIVE_PROFILE="$profile"
  RPS_STRICT_ERRORS="$strict_errors"
  RPS_EXPECTED_CELLS=$((repeats * (4 * ${#read_concurrency[@]} + 3 * ${#transfer_concurrency[@]} + 2 * ${#upload_concurrency[@]})))
  RPS_COMPLETED_CELLS=0
  RPS_FAILED_CELLS=0
  RPS_OVERLOADED_CELLS=0
  RPS_TEXT_FILE="$REPORT_DIR/rps_${TIMESTAMP}.txt"
  RPS_JSON_FILE="$REPORT_DIR/rps_${TIMESTAMP}.json"
  RPS_RAW_DIR="$REPORT_DIR/rps_${TIMESTAMP}_raw"
  RPS_TMP_DIR=$(mktemp -d "/tmp/hps_rps_${TIMESTAMP}.XXXXXX")
  RPS_RESULTS_FILE="$RPS_TMP_DIR/results.tsv"
  mkdir -p "$RPS_RAW_DIR"
  printf 'index\tscenario\tmethod\tpath\tpayload\tconcurrency\tduration_seconds\trepeat\texit_code\tresult\terror\twrk_requests\twrk_status_errors\tstatus_total\tstatus_2xx\tnon_2xx\tunexpected_status\tsocket_connect\tsocket_read\tsocket_write\tsocket_timeout\trps\tavg_latency\tp50\tp90\tp99\tstatus_codes\traw_file\n' \
    > "$RPS_RESULTS_FILE"
  trap cleanup_rps_tmp EXIT

  blue "=== RPS 预检: $RPS_BASE_URL ==="
  local health_body="$RPS_TMP_DIR/health.json"
  local health_raw="$RPS_RAW_DIR/setup_health_${TIMESTAMP}.txt"
  local health_status health_rc
  set +e
  health_status=$(curl -sS --connect-timeout 5 --max-time 15 -o "$health_body" -w '%{http_code}' \
    "$RPS_BASE_URL/api/health" 2> "$health_raw")
  health_rc=$?
  set -e
  {
    printf 'curl_exit=%s http_status=%s\n' "$health_rc" "${health_status:-000}"
    cat "$health_body" 2>/dev/null || true
  } >> "$health_raw"
  cat "$health_raw"
  if [ "$health_rc" -ne 0 ] || [ "$health_status" != "200" ]; then
    rps_preflight_failure "健康检查失败: curl=$health_rc HTTP=${health_status:-000}"
    return 1
  fi

  local suffix="${TIMESTAMP}_$$_${RANDOM}"
  local username="rps_${suffix}"
  local password="Rps_${suffix}_Pass!"
  local register_payload register_body register_raw register_status register_rc
  register_payload=$(python3 - "$username" "$password" <<'PY'
import json
import sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2], "email": sys.argv[1] + "@example.invalid"}))
PY
)
  register_body="$RPS_TMP_DIR/register.json"
  register_raw="$RPS_RAW_DIR/setup_register_${TIMESTAMP}.txt"
  set +e
  register_status=$(curl -sS --connect-timeout 5 --max-time 30 -o "$register_body" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' --data "$register_payload" \
    "$RPS_BASE_URL/api/auth/register" 2> "$register_raw")
  register_rc=$?
  set -e
  {
    printf 'curl_exit=%s http_status=%s username=%s\n' "$register_rc" "${register_status:-000}" "$username"
    echo 'response_body_redacted=true'
  } >> "$register_raw"
  cat "$register_raw"
  if [ "$register_rc" -ne 0 ] || [ "$register_status" != "201" ]; then
    rps_preflight_failure "注册压测用户失败: curl=$register_rc HTTP=${register_status:-000}"
    return 1
  fi

  local token token_parse_rc
  set +e
  token=$(python3 - "$register_body" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream).get("token", ""))
PY
)
  token_parse_rc=$?
  set -e
  if [ "$token_parse_rc" -ne 0 ]; then
    rps_preflight_failure "注册响应不是合法 JSON"
    return 1
  fi
  if [[ ! "$token" =~ ^[A-Za-z0-9._-]+$ ]]; then
    rps_preflight_failure "注册响应缺少合法 Token"
    return 1
  fi

  local payload_1kb="$RPS_TMP_DIR/payload_1kb.bin"
  local payload_1mb="$RPS_TMP_DIR/payload_1mb.bin"
  dd if=/dev/urandom of="$payload_1kb" bs=1024 count=1 status=none
  dd if=/dev/urandom of="$payload_1mb" bs=1048576 count=1 status=none

  local -a seed_files=("$payload_1kb" "$payload_1mb")
  local -a seed_labels=("1KB" "1MB")
  local -a seed_hashes=()
  local -a seed_ids=()
  local seed_index seed_body seed_raw seed_status seed_rc file_hash file_id
  local seed_values seed_parse_rc
  for seed_index in 0 1; do
    seed_body="$RPS_TMP_DIR/seed_${seed_index}.json"
    seed_raw="$RPS_RAW_DIR/setup_seed_${seed_labels[$seed_index]}_${TIMESTAMP}.txt"
    set +e
    seed_status=$(curl -sS --connect-timeout 5 --max-time 120 -o "$seed_body" -w '%{http_code}' \
      -X POST -H 'Content-Type: application/octet-stream' \
      -H "Authorization: Bearer $token" \
      -H "Content-Disposition: attachment; filename=\"rps_seed_${suffix}_${seed_labels[$seed_index]}.bin\"" \
      --data-binary "@${seed_files[$seed_index]}" "$RPS_BASE_URL/api/files/upload" \
      2> "$seed_raw")
    seed_rc=$?
    set -e
    {
      printf 'curl_exit=%s http_status=%s payload=%s\n' "$seed_rc" "${seed_status:-000}" "${seed_labels[$seed_index]}"
      cat "$seed_body" 2>/dev/null || true
    } >> "$seed_raw"
    cat "$seed_raw"
    if [ "$seed_rc" -ne 0 ] || { [ "$seed_status" != "200" ] && [ "$seed_status" != "201" ]; }; then
      rps_preflight_failure "预上传 ${seed_labels[$seed_index]} 失败: curl=$seed_rc HTTP=${seed_status:-000}"
      return 1
    fi
    set +e
    seed_values=$(python3 - "$seed_body" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
print(data.get("file_hash", ""), data.get("file_id", ""))
PY
)
    seed_parse_rc=$?
    set -e
    if [ "$seed_parse_rc" -ne 0 ]; then
      rps_preflight_failure "预上传响应不是合法 JSON"
      return 1
    fi
    read -r file_hash file_id <<< "$seed_values"
    if [[ ! "$file_hash" =~ ^[A-Fa-f0-9]{64}$ ]] || [[ ! "$file_id" =~ ^[1-9][0-9]*$ ]]; then
      rps_preflight_failure "预上传响应缺少 file_hash/file_id"
      return 1
    fi
    seed_hashes+=("$file_hash")
    seed_ids+=("$file_id")
  done

  local -a scenario_names=(
    health auth_me files_list music_list
    upload_1kb upload_1mb
    download_1kb download_1mb range_1mb
  )
  local -a scenario_methods=(GET GET GET GET POST POST GET GET GET)
  local -a scenario_paths=(
    "/api/health"
    "/api/auth/me"
    "/api/files?offset=0&limit=20"
    "/api/music/library?offset=0&limit=20"
    "/api/files/upload"
    "/api/files/upload"
    "/api/files/by-hash/${seed_hashes[0]}/download"
    "/api/files/by-hash/${seed_hashes[1]}/download"
    "/api/files/${seed_ids[1]}/stream"
  )
  local -a scenario_payloads=(none none none none 1KB 1MB 1KB 1MB 1KB-range)
  local -a scenario_classes=(read read read read upload upload transfer transfer transfer)
  local -a scenario_bodies=("" "" "" "" "$payload_1kb" "$payload_1mb" "" "" "")
  local -a scenario_types=("" "" "" "" application/octet-stream application/octet-stream "" "" "")
  local -a scenario_tokens=("" "$token" "$token" "$token" "$token" "$token" "$token" "$token" "$token")
  local -a scenario_ranges=("" "" "" "" "" "" "" "" "bytes=0-1023")
  local -a scenario_codes=(200 200 200 200 201 201 200 200 206)
  local -a scenario_unique_bodies=(0 0 0 0 1 1 0 0 0)
  local -a scenario_dispositions=(
    "" "" "" ""
    "attachment; filename=\"rps_upload_${suffix}_1kb.bin\""
    "attachment; filename=\"rps_upload_${suffix}_1mb.bin\""
    "" "" ""
  )

  blue "=== RPS 矩阵: profile=$profile cells=$RPS_EXPECTED_CELLS ==="
  local cell_index=0
  local scenario_index repeat concurrency cell_duration request_delay_ms
  local -a active_concurrency=()
  for repeat in $(seq 1 "$repeats"); do
    for scenario_index in "${!scenario_names[@]}"; do
      case "${scenario_classes[$scenario_index]}" in
        read)
          active_concurrency=("${read_concurrency[@]}")
          cell_duration="$duration"
          request_delay_ms=0
          ;;
        transfer)
          active_concurrency=("${transfer_concurrency[@]}")
          cell_duration="$duration"
          request_delay_ms=0
          ;;
        upload)
          active_concurrency=("${upload_concurrency[@]}")
          cell_duration="$RPS_UPLOAD_DURATION_SECONDS"
          request_delay_ms="$RPS_UPLOAD_DELAY_MILLISECONDS"
          ;;
      esac
      for concurrency in "${active_concurrency[@]}"; do
        cell_index=$((cell_index + 1))
        run_rps_cell "$cell_index" "${scenario_names[$scenario_index]}" \
          "${scenario_methods[$scenario_index]}" "${scenario_paths[$scenario_index]}" \
          "${scenario_payloads[$scenario_index]}" "${scenario_bodies[$scenario_index]}" \
          "${scenario_types[$scenario_index]}" "${scenario_tokens[$scenario_index]}" \
          "${scenario_ranges[$scenario_index]}" "${scenario_codes[$scenario_index]}" \
          "${scenario_dispositions[$scenario_index]}" "$concurrency" "$cell_duration" "$repeat" "$strict_errors" \
          "${scenario_unique_bodies[$scenario_index]}" "${suffix}_${cell_index}" "$request_delay_ms"
      done
    done
  done

  write_rps_reports "$profile" "$strict_errors" "passed" "-"
  cleanup_rps_tmp
  trap - EXIT
  green "文本报告: $RPS_TEXT_FILE"
  green "JSON 报告: $RPS_JSON_FILE"
  green "原始输出: $RPS_RAW_DIR"
  if [ "$RPS_COMPLETED_CELLS" -ne "$RPS_EXPECTED_CELLS" ] || [ "$RPS_FAILED_CELLS" -ne 0 ]; then
    red "RPS 门禁失败: 完成 $RPS_COMPLETED_CELLS/$RPS_EXPECTED_CELLS，失败 $RPS_FAILED_CELLS"
    return 1
  fi
  if [ "$RPS_OVERLOADED_CELLS" -gt 0 ]; then
    yellow "过载观测完成: $RPS_OVERLOADED_CELLS 个单元出现 HTTP/Socket 错误"
  else
    green "RPS 矩阵全部 $RPS_EXPECTED_CELLS 个单元通过"
  fi
}

cmd_diff() {
  require_tools python3 find sort
  local name="${1:-micro}"
  [ "$name" = "load" ] && name="rps"
  case "$name" in
    micro|qps|rps) ;;
    *) red "错误: diff 仅支持 micro、qps、rps（load 是 rps 的别名）"; return 2 ;;
  esac
  local -a reports=()
  mapfile -t reports < <(
    find "$REPORT_DIR" -maxdepth 1 -type f \
      -regextype posix-extended -regex ".*/${name}_[0-9]{8}_[0-9]{6}\\.json" -printf '%p\n' | sort
  )
  if [ "${#reports[@]}" -lt 2 ]; then
    red "错误: 至少需要两个带时间戳的 $name JSON 报告"
    return 1
  fi
  local previous="${reports[$((${#reports[@]} - 2))]}"
  local current="${reports[$((${#reports[@]} - 1))]}"
  python3 - "$previous" "$current" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    previous = json.load(stream)
with open(sys.argv[2], encoding="utf-8") as stream:
    current = json.load(stream)
print(f"前一报告: {sys.argv[1]}")
print(f"当前报告: {sys.argv[2]}")
print(f"前一汇总: {json.dumps(previous.get('summary', {}), ensure_ascii=False)}")
print(f"当前汇总: {json.dumps(current.get('summary', {}), ensure_ascii=False)}")
if current.get("type") == "rps" and previous.get("type") == "rps":
    def key(row):
        return (row.get("scenario"), row.get("concurrency"), row.get("repeat"))
    old = {key(row): row for row in previous.get("results", [])}
    print("\n共同场景 RPS 变化:")
    for row in current.get("results", []):
        before = old.get(key(row))
        if not before or not before.get("rps"):
            continue
        delta = (row.get("rps", 0) - before["rps"]) / before["rps"] * 100
        print(f"  {row['scenario']} c={row['concurrency']}: {before['rps']:.1f} -> {row['rps']:.1f} ({delta:+.2f}%)")
PY
}

cmd_gen_data() {
  require_tools dd
  local data_dir="$PROJECT_ROOT/data/bench"
  mkdir -p "$data_dir"
  blue "=== 生成测试数据（$data_dir）==="
  local size human_size file
  for size in 1024 1048576 10485760 20971520 31457280 41943040 52428800 62914560 73400320 83886080 94371840 104857600; do
    if [ "$size" -ge 1048576 ]; then
      human_size="$((size / 1048576))MB"
    else
      human_size="${size}B"
    fi
    file="$data_dir/test_${human_size}.bin"
    if [ ! -f "$file" ]; then
      dd if=/dev/urandom of="$file" bs="$size" count=1 status=none
      green "已生成: $file ($human_size)"
    else
      yellow "已存在: $file ($human_size)"
    fi
  done
  ls -lh "$data_dir/"
}

cmd_check() {
  local failed=0
  require_tools bash xmake python3 timeout curl wrk find sort awk dd || failed=1
  discover_qps_targets
  if [ "${#QPS_EXPECTED_TARGETS[@]}" -eq 0 ]; then
    red "离线检查失败: 未发现 QPS 源文件"
    failed=1
  else
    green "QPS 源目标: ${#QPS_EXPECTED_TARGETS[@]} 个"
  fi
  if validate_qps_artifacts; then
    green "当前 QPS 构建产物与源文件一致"
  else
    yellow "当前构建产物尚未同步；qps 命令会先编译并严格拒绝 missing/stale"
    [ "${#QPS_MISSING_TARGETS[@]}" -gt 0 ] && yellow "缺失: ${QPS_MISSING_TARGETS[*]}"
    [ "${#QPS_STALE_TARGETS[@]}" -gt 0 ] && yellow "陈旧: ${QPS_STALE_TARGETS[*]}"
  fi
  green "RPS smoke 矩阵: 16 单元"
  green "RPS full 矩阵: 40 单元"
  green "RPS overload 矩阵: 25 单元"
  if [ "$failed" -ne 0 ]; then
    return 1
  fi
  green "离线检查通过（未访问 RPS_BASE_URL）"
}

usage() {
  cat <<EOF
用法: $(basename "$0") <子命令> [profile|选项]

子命令:
  micro              运行 Google Benchmark 微基准
  qps [smoke|full]    校验并运行全部 benchmark/qps_*.cpp 对应目标
  rps [profile]       压测已部署同源入口，profile=smoke|full|overload
  load [profile]      rps 的兼容别名
  diff <类型>         对比最近两个时间戳 JSON 报告
  gen-data            生成 benchmark 数据文件
  build [--debug]     编译 benchmark 二进制
  check               仅执行本地依赖与目标发现检查
  -h, --help          显示帮助

主要环境变量:
  QPS_PROFILE=smoke|full
  QPS_TIMEOUT_SECONDS=<秒>
  RPS_BASE_URL=http://127.0.0.1:18080
    未设置时安全读取 .env 的 HPS_HTTP_PORT，缺失时使用 18080；默认端口 8080 会被拒绝
  RPS_PROFILE=smoke|full|overload
  RPS_DURATION_SECONDS=<秒>  RPS_REPEATS=<次数>
  RPS_READ_CONCURRENCY="1 10 ..."
  RPS_TRANSFER_CONCURRENCY="1 10 ..."
  RPS_UPLOAD_CONCURRENCY="1 5 ..."
  RPS_UPLOAD_DURATION_SECONDS=<秒>
  RPS_UPLOAD_DELAY_MILLISECONDS=<毫秒>
EOF
}

handle_menu_choice() {
  case "$1" in
    1) cmd_micro ;;
    2) cmd_qps ;;
    3) cmd_rps ;;
    4) cmd_diff ;;
    5) cmd_gen_data ;;
    6) cmd_build ;;
    7) cmd_check ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "micro:微基准测试"
    "qps:模块 QPS 测试"
    "rps:端到端 HTTP RPS 压测"
    "diff:时间戳报告对比"
    "gen-data:生成测试数据"
    "build:编译 benchmark"
    "check:离线检查"
  )
  menu_loop "基准测试工具（$PROJECT_ROOT）" "${items[@]}"
}

main() {
  if [ "$#" -gt 0 ]; then
    case "$1" in
      micro) shift; cmd_micro "$@" ;;
      qps) shift; cmd_qps "$@" ;;
      rps) shift; cmd_rps "$@" ;;
      load) shift; cmd_rps "$@" ;;
      diff) shift; cmd_diff "$@" ;;
      gen-data) shift; cmd_gen_data "$@" ;;
      build) shift; cmd_build "$@" ;;
      check) shift; cmd_check "$@" ;;
      -h|--help) usage ;;
      *) red "未知子命令: $1"; usage; return 1 ;;
    esac
  else
    menu
  fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
