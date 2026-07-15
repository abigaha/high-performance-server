#!/usr/bin/env bash
# ============================================================
# benchmark.sh — 性能基准测试（微基准 + 负载测试）+ 基线管理
# ============================================================
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"
cd "$PROJECT_ROOT"

# 设置共享库路径
export LD_LIBRARY_PATH="$PROJECT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BASELINE_DIR="$PROJECT_ROOT/benchmark/baseline"
REPORT_DIR="$PROJECT_ROOT/benchmark/reports"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
HOSTNAME=$(hostname 2>/dev/null || echo "unknown")
mkdir -p "$REPORT_DIR"

ensure_binaries() {
  local missing=0
  for b in wrk; do
    if ! command -v "$b" &>/dev/null; then
      red "错误: $b 未安装"
      if [ "$b" = "wrk" ]; then
        echo "  请安装: sudo apt-get install wrk"
      fi
      missing=1
    fi
  done
  if [ "$missing" -ne 0 ]; then
    exit 1
  fi
}

env_info() {
  cat <<EOF
{
  "timestamp": "$TIMESTAMP",
  "git_hash": "$GIT_HASH",
  "hostname": "$HOSTNAME",
  "cpu_model": "$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //' || echo 'unknown')",
  "cpu_cores": "$(nproc 2>/dev/null || echo 'unknown')",
  "memory_kb": "$(grep MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}' || echo 'unknown')"
}
EOF
}

save_text_report() {
  local name="$1"
  local content="$2"
  local report_file="$REPORT_DIR/${name}_${TIMESTAMP}.txt"
  mkdir -p "$REPORT_DIR"
  {
    echo "=========================================="
    echo "  性能测试报告: $name"
    echo "  时间: $TIMESTAMP"
    echo "  Git:  $GIT_HASH"
    echo "  主机: $HOSTNAME"
    echo "  CPU:  $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //' || echo 'unknown')"
    echo "  核心: $(nproc 2>/dev/null || echo 'unknown')"
    echo "=========================================="
    echo ""
    echo "$content"
  } > "$report_file"
  green "报告已保存: $report_file"
  ln -sf "$report_file" "$REPORT_DIR/${name}_latest.txt"
}

save_baseline() {
  local name="$1"
  local data="$2"
  local baseline_file="$BASELINE_DIR/${name}.json"
  mkdir -p "$BASELINE_DIR"
  if [ -f "$baseline_file" ]; then
    local prev_ts; prev_ts=$(grep -o '"timestamp": "[^"]*"' "$baseline_file" | head -1)
    echo "$data" | python3 -c "
import json,sys
cur = json.load(sys.stdin)
try:
    with open('$baseline_file') as f:
        prev = json.load(f)
except:
    prev = {}
print(json.dumps({'previous': prev, 'current': cur, 'baseline_name': '$name'}, indent=2))
" > "$baseline_file"
  else
    echo "$data" > "$baseline_file"
  fi
}

load_baseline() {
  local name="$1"
  local baseline_file="$BASELINE_DIR/${name}.json"
  if [ -f "$baseline_file" ]; then
    python3 -c "
import json
with open('$baseline_file') as f:
    data = json.load(f)
if 'previous' in data:
    print(json.dumps(data['previous']))
else:
    print(json.dumps(data))
"
  fi
}

cmd_build() {
  blue "=== 编译 benchmark（Release 模式）==="
  local mode="release"
  if [ "${1:-}" = "--debug" ]; then
    mode="debug"
    yellow "警告: debug 模式数据不准确，仅用于验证编译"
  fi
  xmake f -m "$mode" -y >/dev/null 2>&1
  xmake -j"$(nproc)" 2>&1 | grep -v '^checking\|^check\|^>'
  green "编译完成（${mode}）"
}

cmd_micro() {
  ensure_binaries
  cmd_build "$@"

  blue "=== 微基准测试 ==="
  local bench_bins
  bench_bins=$(find "$PROJECT_ROOT/bin" -name 'bench_*' -type f 2>/dev/null | sort || true)

  if [ -z "$bench_bins" ]; then
    red "未找到 benchmark 二进制（bin/bench_bench_*），请先编译"
    exit 1
  fi

  local all_results="{}"
  local first=1
  for bin in $bench_bins; do
    local name; name=$(basename "$bin")
    yellow "  运行: $name"
    local output
    output=$("$bin" --benchmark_format=json 2>/dev/null || "$bin" 2>/dev/null || true)
    if [ -n "$output" ]; then
      if [ "$first" -eq 1 ]; then
        all_results=$(echo "$output" | python3 -c "
import json,sys
try:
    d = json.load(sys.stdin)
    d['benchmark_name'] = '$name'
    print(json.dumps(d))
except:
    print('{}')
" 2>/dev/null || echo "{}")
        first=0
      else
        all_results=$(echo "$all_results" | python3 -c "
import json,sys
d = json.load(sys.stdin)
try:
    import subprocess
    result = subprocess.run(['python3', '-c', '''
import json,sys
cur = json.load(sys.stdin)
cur['benchmark_name'] = \"'\"'\"'$name'\"'\"'\"
print(json.dumps(cur))
'''], input='$output', capture_output=True, text=True)
    if result.stdout:
        d['benchmarks'] = d.get('benchmarks', []) + json.loads(result.stdout).get('benchmarks', [])
except:
    pass
print(json.dumps(d))
" 2>/dev/null || echo "{}")
      fi
    fi
  done

  echo ""
  blue "=== 微基准测试结果 ==="
  printf "%-40s %15s %15s %15s\n" "测试名" "均值" "中位数" "标准差"
  printf "%-40s %15s %15s %15s\n" "$(printf '=%.0s' {1..40})" "$(printf '=%.0s' {1..15})" "$(printf '=%.0s' {1..15})" "$(printf '=%.0s' {1..15})"
  for bin in $bench_bins; do
    local name; name=$(basename "$bin")
    "$bin" 2>/dev/null | while IFS= read -r line; do
      if echo "$line" | grep -qE '^[-_A-Za-z0-9/]+\s+'; then
        echo "$line"
      fi
    done
  done

  local env; env=$(env_info)
  local full_data
  full_data=$(echo "{}" | python3 -c "
import json,sys
d = json.load(sys.stdin)
d['type'] = 'micro'
d['environment'] = json.loads('$env')
d['results'] = {}
print(json.dumps(d, indent=2))
")
  save_baseline "micro_latest" "$full_data"
  green "微基准测试完成，结果已保存到 $BASELINE_DIR/micro_latest.json"

  # 生成文本报告
  local report_file="$REPORT_DIR/micro_${TIMESTAMP}.txt"
  mkdir -p "$REPORT_DIR"
  {
    echo "=========================================="
    echo "  微基准测试报告"
    echo "  时间: $TIMESTAMP"
    echo "  Git:  $GIT_HASH"
    echo "  主机: $HOSTNAME"
    echo "  CPU:  $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //' || echo 'unknown')"
    echo "  核心: $(nproc 2>/dev/null || echo 'unknown')"
    echo "=========================================="
    echo ""
    for bin in $bench_bins; do
      local name; name=$(basename "$bin")
      echo "--- $name ---"
      "$bin" 2>/dev/null | while IFS= read -r line; do
        if echo "$line" | grep -qE '^[-_A-Za-z0-9/]+\s+'; then
          echo "  $line"
        fi
      done
      echo ""
    done
  } > "$report_file"
  green "报告已保存: $report_file"
  ln -sf "$report_file" "$REPORT_DIR/micro_latest.txt"
}

cmd_load() {
  ensure_binaries
  cmd_build "$@"

  local server_bin="$PROJECT_ROOT/bin/high-performance-server"
  if [ ! -f "$server_bin" ]; then
    red "未找到服务器二进制，请编译"
    exit 1
  fi

  local port=9090

  yellow "启动服务器（端口 $port）..."
  "$server_bin" --port "$port" --threads "$(nproc)" &
  g_bench_server_pid=$!

  for i in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:$port/api/health" >/dev/null 2>&1; then
      green "服务器已就绪"
      break
    fi
    sleep 0.5
  done

  cleanup() {
    kill "${g_bench_server_pid:-}" 2>/dev/null || true
    wait "${g_bench_server_pid:-}" 2>/dev/null || true
  }
  trap cleanup EXIT

  local urls=(
    "http://127.0.0.1:$port/api/health"
    "http://127.0.0.1:$port/api/users/1"
  )

  local concurrency_levels=(1 10 50 100 500 1000)
  local payloads=("1KB" "1MB" "10MB")

  blue "=== 负载测试 ==="
  local load_results="[]"

  for url in "${urls[@]}"; do
    for conn in "${concurrency_levels[@]}"; do
      local path_part; path_part=$(echo "$url" | sed 's|.*/api/|/api/|')
      for duration in 15; do
        yellow "  $path_part  并发=$conn  持续时间=${duration}s"
        local output
        local wrk_threads=$(( conn < $(nproc) ? conn : $(nproc) ))
        output=$(wrk -t"$wrk_threads" -c"$conn" -d"${duration}s" --latency "$url" 2>/dev/null || true)
        if [ -n "$output" ]; then
          local rps; rps=$(echo "$output" | grep 'Requests/sec' | awk '{print $2}')
          local avg_lat; avg_lat=$(echo "$output" | grep 'Latency' | head -1 | awk '{print $2}' | sed 's/ms//')
          local p50; p50=$(echo "$output" | grep '50%' | awk '{print $2}')
          local p90; p90=$(echo "$output" | grep '90%' | awk '{print $2}')
          local p99; p99=$(echo "$output" | grep '99%' | awk '{print $2}')
          printf "    RPS: %10s  Lat(avg): %8s  p50: %8s  p90: %8s  p99: %8s\n" \
            "${rps:-N/A}" "${avg_lat:-N/A}" "${p50:-N/A}" "${p90:-N/A}" "${p99:-N/A}"

          load_results=$(echo "$load_results" | python3 -c "
import json,sys
d = json.load(sys.stdin)
d.append({
    'url': '$url',
    'concurrency': $conn,
    'duration': '${duration}s',
    'rps': ${rps:-0},
    'avg_latency_ms': '${avg_lat:-0}',
    'p50': '$p50',
    'p90': '$p90',
    'p99': '$p99'
})
print(json.dumps(d))
")
        fi
      done
    done
  done

  local env_json; env_json=$(env_info)
  export BENCH_ENV_JSON="$env_json"
  local full_data
  full_data=$(echo "{}" | python3 -c "
import json,sys,os
d = json.load(sys.stdin)
d['type'] = 'load'
d['environment'] = json.loads(os.environ['BENCH_ENV_JSON'])
d['results'] = $load_results
print(json.dumps(d, indent=2))
")
  save_baseline "load_latest" "$full_data"
  green "负载测试完成，结果已保存到 $BASELINE_DIR/load_latest.json"

  # 生成文本报告
  local report_file="$REPORT_DIR/load_${TIMESTAMP}.txt"
  mkdir -p "$REPORT_DIR"
  {
    echo "=========================================="
    echo "  负载测试报告"
    echo "  时间: $TIMESTAMP"
    echo "  Git:  $GIT_HASH"
    echo "  主机: $HOSTNAME"
    echo "  CPU:  $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //' || echo 'unknown')"
    echo "  核心: $(nproc 2>/dev/null || echo 'unknown')"
    echo "=========================================="
    echo ""
    printf "%-25s %10s %10s %12s %12s %12s\n" "端点" "并发" "RPS" "平均延迟" "p50" "p90" "p99"
    echo "$(printf '=%.0s' {1..95})"
    echo "$load_results" | python3 -c "
import json,sys
results = json.load(sys.stdin)
for r in sorted(results, key=lambda x: (x.get('url',''), x.get('concurrency',0))):
    url = r.get('url','').split('/api/')[-1] if '/api/' in r.get('url','') else r.get('url','')
    print(f\"{url:25s} {r.get('concurrency',0):10d} {r.get('rps',0):10.1f} {str(r.get('avg_latency_ms','N/A')):>10s}ms {str(r.get('p50','N/A')):>10s} {str(r.get('p90','N/A')):>10s} {str(r.get('p99','N/A')):>10s}\")
"
  } > "$report_file"
  green "报告已保存: $report_file"
  ln -sf "$report_file" "$REPORT_DIR/load_latest.txt"
}

cmd_diff() {
  local name="${1:-micro}"

  if [ ! -f "$BASELINE_DIR/${name}_latest.json" ]; then
    red "未找到基线数据: $BASELINE_DIR/${name}_latest.json"
    exit 1
  fi

  blue "=== 基线对比: $name ==="
  python3 -c "
import json
with open('$BASELINE_DIR/${name}_latest.json') as f:
    data = json.load(f)
env = data.get('environment', {})
print(f'环境: {env.get(\"hostname\", \"?\")} / {env.get(\"cpu_model\", \"?\")}')
print(f'时间: {env.get(\"timestamp\", \"?\")}  Git: {env.get(\"git_hash\", \"?\")}')
print()

results = data.get('results', {})
if isinstance(results, dict):
    for bench_name, metrics in results.items():
        print(f'--- {bench_name} ---')
        for key, val in metrics.items():
            print(f'  {key}: {val}')
elif isinstance(results, list):
    for r in results:
        print(f'{r.get(\"url\", \"?\")} 并发={r.get(\"concurrency\", \"?\")}  RPS={r.get(\"rps\", \"?\")}  '
              f'p50={r.get(\"p50\", \"?\")}  p90={r.get(\"p90\", \"?\")}  p99={r.get(\"p99\", \"?\")}')
"
}

cmd_gen_data() {
  local data_dir="$PROJECT_ROOT/data/bench"
  mkdir -p "$data_dir"
  blue "=== 生成测试数据（$data_dir）==="

  for size in 1024 1048576 10485760 20971520 31457280 41943040 52428800 62914560 73400320 83886080 94371840 104857600; do
    local human_size
    if [ "$size" -ge 1048576 ]; then
      human_size="$((size / 1048576))MB"
    else
      human_size="${size}B"
    fi
    local file="$data_dir/test_${human_size}.bin"
    if [ ! -f "$file" ]; then
      dd if=/dev/urandom of="$file" bs="$size" count=1 2>/dev/null
      green "  已生成: $file ($human_size)"
    else
      yellow "  已存在: $file ($human_size)"
    fi
  done

  ls -lh "$data_dir/"
  green "测试数据生成完成"
}

usage() {
  cat <<EOF
用法: $(basename "$0") <子命令> [选项]

子命令:
  micro      运行微基准测试（Google Benchmark）
  load       运行负载测试（wrk HTTP 压测）
  diff       查看基线对比（默认 micro）
  gen-data   生成测试数据文件
  build      编译 benchmark 二进制（Release 模式）
  -h, --help 显示帮助

选项:
  --debug    使用 Debug 模式编译（仅验证编译，数据不准确）
EOF
}

handle_menu_choice() {
  case "$1" in
    1) cmd_micro ;;
    2) cmd_load ;;
    3) cmd_diff ;;
    4) cmd_gen_data ;;
    5) cmd_build ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "micro:微基准测试"
    "load:负载测试"
    "diff:基线对比"
    "gen-data:生成测试数据"
    "build:编译 benchmark"
  )
  menu_loop "基准测试工具（$PROJECT_ROOT）" "${items[@]}"
}

if [ $# -gt 0 ]; then
  case "$1" in
    micro) shift; cmd_micro "$@" ;;
    load) shift; cmd_load "$@" ;;
    diff) shift; cmd_diff "${1:-}" ;;
    gen-data) cmd_gen_data ;;
    build) shift; cmd_build "$@" ;;
    -h|--help) usage; exit 0 ;;
    *)
      red "未知子命令: $1"
      usage
      exit 1
      ;;
  esac
else
  menu
fi
