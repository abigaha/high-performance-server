#!/usr/bin/env bash
# ============================================================
# benchmark.sh - 微基准、模块 QPS 与端到端 HTTP RPS 测试
# ============================================================
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"
cd "$PROJECT_ROOT"

export LD_LIBRARY_PATH="$PROJECT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

REPORT_DIR="$PROJECT_ROOT/benchmark/reports"
REPORT_ROOT="$PROJECT_ROOT/benchmark/report"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BENCH_HOSTNAME=$(hostname 2>/dev/null || echo "unknown")
CPU_MODEL=$(awk -F ': ' '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null || echo "unknown")
CPU_CORES=$(nproc 2>/dev/null || echo "unknown")
MEMORY_KB=$(awk '/MemTotal/{print $2; exit}' /proc/meminfo 2>/dev/null || echo "unknown")
RPS_REQUESTED_PROFILES=()

has_untracked_business_files() {
  local path
  while IFS= read -r -d '' path; do
    case "$path" in
      benchmark/report|benchmark/report/*) ;;
      *) return 0 ;;
    esac
  done < <(git ls-files --others --exclude-standard -z 2>/dev/null)
  return 1
}

if git diff --quiet --ignore-submodules HEAD -- . ':(exclude)benchmark/report/**' 2>/dev/null && \
   ! has_untracked_business_files; then
  GIT_DIRTY=false
else
  GIT_DIRTY=true
fi

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

is_finite_positive_number() {
  python3 - "$1" <<'PY'
import math
import sys

try:
    value = float(sys.argv[1])
except ValueError:
    raise SystemExit(1)
raise SystemExit(0 if math.isfinite(value) and value > 0 else 1)
PY
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

is_run_id() {
  [[ "$1" =~ ^[0-9]{8}_[0-9]{6}$ ]]
}

parse_run_options() {
  BENCHMARK_RUN_ID=""
  BENCHMARK_RESUME=0
  BENCHMARK_BUILD_MODE="release"
  local explicit_run_id=""
  local resume_run_id=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --debug)
        if [ "$BENCHMARK_BUILD_MODE" = "debug" ]; then
          red "错误: --debug 不能重复指定"
          return 2
        fi
        BENCHMARK_BUILD_MODE="debug"
        ;;
      --run-id)
        if [ "$#" -lt 2 ]; then
          red "错误: --run-id 缺少运行 ID"
          return 2
        fi
        if [ -n "$explicit_run_id" ]; then
          red "错误: --run-id 不能重复指定"
          return 2
        fi
        explicit_run_id="$2"
        shift
        ;;
      --resume)
        if [ "$#" -lt 2 ]; then
          red "错误: --resume 缺少运行 ID"
          return 2
        fi
        if [ -n "$resume_run_id" ]; then
          red "错误: --resume 不能重复指定"
          return 2
        fi
        resume_run_id="$2"
        shift
        ;;
      *)
        red "错误: 不支持的运行选项: $1"
        return 2
        ;;
    esac
    shift
  done

  if [ -n "$explicit_run_id" ] && [ -n "$resume_run_id" ]; then
    red "错误: --run-id 与 --resume 互斥"
    return 2
  fi
  if [ -n "$explicit_run_id" ]; then
    BENCHMARK_RUN_ID="$explicit_run_id"
  elif [ -n "$resume_run_id" ]; then
    BENCHMARK_RUN_ID="$resume_run_id"
    BENCHMARK_RESUME=1
  else
    BENCHMARK_RUN_ID="$TIMESTAMP"
  fi
  if ! is_run_id "$BENCHMARK_RUN_ID"; then
    red "错误: 运行 ID 必须符合 YYYYMMDD_HHMMSS"
    return 2
  fi
}

ensure_new_run_available() {
  local kind="$1"
  local run_id="$2"
  shift 2
  local target run_dir

  [ "$BENCHMARK_RESUME" -eq 1 ] && return 0
  for target in "$@"; do
    run_dir="$REPORT_ROOT/$kind/$target/$run_id"
    if [ -e "$run_dir" ]; then
      red "错误: 新运行不能覆盖已有目录: $run_dir"
      return 1
    fi
  done
}

is_safe_report_component() {
  [ "$1" != "." ] && [ "$1" != ".." ] && [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

report_run_dir_is_safe() {
  local kind="$1"
  local target="$2"
  local profile="$3"
  local run_id="$4"
  local run_dir="$5"
  local allow_missing="${6:-0}"
  local selector expected_run_dir component
  local -a components=()

  case "$kind" in
    micro|qps)
      selector="$target"
      ;;
    rps)
      selector="$profile"
      ;;
    *)
      red "错误: 报告类型不是预期安全值: $kind"
      return 1
      ;;
  esac
  if ! is_safe_report_component "$selector" || ! is_run_id "$run_id"; then
    red "错误: 报告路径包含不安全组件"
    return 1
  fi

  expected_run_dir="$REPORT_ROOT/$kind/$selector/$run_id"
  if [ "$run_dir" != "$expected_run_dir" ]; then
    red "错误: 报告运行目录不符合固定布局: $run_dir"
    return 1
  fi
  components=(
    "$PROJECT_ROOT/benchmark"
    "$REPORT_ROOT"
    "$REPORT_ROOT/$kind"
    "$REPORT_ROOT/$kind/$selector"
    "$run_dir"
  )
  for component in "${components[@]}"; do
    if [ -L "$component" ]; then
      red "错误: 报告路径包含符号链接，拒绝访问: $component"
      return 1
    fi
    if [ -e "$component" ]; then
      if [ ! -d "$component" ]; then
        red "错误: 报告路径不是目录，拒绝访问: $component"
        return 1
      fi
    elif [ "$allow_missing" -ne 1 ]; then
      red "错误: 报告路径不存在，拒绝访问: $component"
      return 1
    fi
  done
}

resume_path_is_safe() {
  local kind="$1"
  local target="$2"
  local run_id="$3"
  local run_dir="$REPORT_ROOT/$kind/$target/$run_id"

  report_run_dir_is_safe "$kind" "$target" "" "$run_id" "$run_dir" 1
}

is_single_link_regular_file() {
  local path="$1"
  [ -f "$path" ] && [ ! -L "$path" ] && [ "$(stat -c '%h' "$path" 2>/dev/null)" = 1 ]
}

create_artifact_temp_file() {
  local kind="$1"
  local target="$2"
  local run_id="$3"
  local run_dir="$4"
  local artifact="$5"
  local temp_file

  report_run_dir_is_safe "$kind" "$target" "" "$run_id" "$run_dir" || return 1
  temp_file=$(mktemp "$run_dir/.${artifact}.XXXXXX") || return 1
  if ! is_single_link_regular_file "$temp_file"; then
    red "错误: artifact 临时文件不是单链接普通文件: $temp_file"
    return 1
  fi
  printf '%s\n' "$temp_file"
}

artifact_destination_is_replaceable() {
  local artifact_path="$1"

  if ! python3 - "$artifact_path" <<'PY'
import os
import stat
import sys

try:
    mode = os.lstat(sys.argv[1]).st_mode
except FileNotFoundError:
    raise SystemExit(0)
except OSError:
    raise SystemExit(1)

raise SystemExit(0 if stat.S_ISREG(mode) or stat.S_ISLNK(mode) else 1)
PY
  then
    red "错误: artifact 目标不是可安全替换的文件: $artifact_path"
    return 1
  fi
}

replace_artifact_file() {
  local temp_file="$1"
  local artifact_path="$2"

  artifact_destination_is_replaceable "$artifact_path" || return 1
  mv -T -f -- "$temp_file" "$artifact_path"
}

publish_artifact_file() {
  local kind="$1"
  local target="$2"
  local run_id="$3"
  local run_dir="$4"
  local artifact="$5"
  local temp_file="$6"
  local artifact_path="$run_dir/$artifact"

  report_run_dir_is_safe "$kind" "$target" "" "$run_id" "$run_dir" || return 1
  if ! is_single_link_regular_file "$temp_file"; then
    red "错误: artifact 临时文件在发布前不安全: $temp_file"
    return 1
  fi
  replace_artifact_file "$temp_file" "$artifact_path" || return 1
  if ! report_run_dir_is_safe "$kind" "$target" "" "$run_id" "$run_dir" || \
    ! is_single_link_regular_file "$artifact_path"; then
    red "错误: artifact 发布后运行目录或文件不安全: $artifact_path"
    return 1
  fi
}

prepare_target_run_dir() {
  local kind="$1"
  local target="$2"
  local run_id="$3"
  local run_dir="$REPORT_ROOT/$kind/$target/$run_id"

  report_run_dir_is_safe "$kind" "$target" "" "$run_id" "$run_dir" 1 || return 1
  if [ "$BENCHMARK_RESUME" -eq 1 ]; then
    mkdir -p "$run_dir"
  else
    mkdir -p "$(dirname "$run_dir")"
    if ! mkdir "$run_dir"; then
      red "错误: 新运行不能创建目标目录: $run_dir"
      return 1
    fi
  fi
  report_run_dir_is_safe "$kind" "$target" "" "$run_id" "$run_dir" || return 1
  printf '%s\n' "$run_dir"
}

make_run_fingerprint() {
  python3 - "$@" <<'PY'
import hashlib
import json
import sys

values = sys.argv[1:]
if len(values) % 2:
    raise SystemExit("fingerprint arguments must be key/value pairs")
inputs = {values[index]: values[index + 1] for index in range(0, len(values), 2)}
payload = json.dumps(inputs, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
print(hashlib.sha256(payload.encode("utf-8")).hexdigest())
PY
}

safe_value_sha256() {
  python3 - "$1" <<'PY'
import hashlib
import sys

print(hashlib.sha256(sys.argv[1].encode("utf-8")).hexdigest())
PY
}

validate_bench_flags() {
  local value="$1"
  local -a flags=()
  local flag name

  read -r -a flags <<< "$value"
  for flag in "${flags[@]}"; do
    case "$flag" in
      -*)
        name="${flag%%=*}"
        name="${name#--}"
        name="${name#-}"
        name="${name,,}"
        case "$name" in
          *token*|*password*|*secret*|*credential*|*api-key*|*api_key*|*apikey*)
            red "错误: BENCH_FLAGS 包含敏感参数，拒绝执行"
            return 1
            ;;
        esac
        ;;
    esac
  done
}

next_manifest_attempt() {
  local manifest_file="$1"
  local kind="$2"
  local target="$3"
  local profile="$4"
  local run_id="$5"
  local run_dir="$6"

  python3 - "$manifest_file" "$kind" "$target" "$profile" "$run_id" "$run_dir" "$REPORT_ROOT" <<'PY'
import json
import os
import stat
import sys

manifest_file, kind, target, profile, run_id, run_dir, report_root = sys.argv[1:]

def has_symlink_component(path):
    current = os.path.abspath(path)
    while True:
        if os.path.islink(current):
            return True
        parent = os.path.dirname(current)
        if parent == current:
            return False
        current = parent

def has_expected_location(manifest):
    if kind not in {"micro", "qps", "rps"}:
        return False
    if (
        manifest.get("kind") != kind
        or manifest.get("target") != target
        or manifest.get("profile") != profile
        or manifest.get("run_id") != run_id
    ):
        return False
    selector = profile if kind == "rps" else target
    expected_run_dir = os.path.join(os.path.abspath(report_root), kind, selector, run_id)
    return (
        os.path.abspath(run_dir) == expected_run_dir
        and os.path.abspath(manifest_file) == os.path.join(expected_run_dir, "manifest.json")
    )

try:
    if has_symlink_component(sys.argv[1]):
        raise ValueError("manifest path contains a symlink")
    metadata = os.lstat(sys.argv[1])
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise ValueError("manifest must be a single-link regular file")
    with open(sys.argv[1], encoding="utf-8") as stream:
        manifest = json.load(stream)
    if not isinstance(manifest, dict):
        raise ValueError("manifest must be an object")
    if not has_expected_location(manifest):
        raise ValueError("manifest location does not match its identity")
    attempt = manifest.get("attempt", 0)
    if not isinstance(attempt, int) or isinstance(attempt, bool):
        raise ValueError("attempt must be an integer")
    print(max(0, attempt) + 1)
except (OSError, UnicodeError, ValueError, TypeError, json.JSONDecodeError):
    print(1)
PY
}

manifest_is_complete() {
  local manifest_file="$1"
  local kind="$2"
  local target_filter="$3"
  local profile_filter="$4"
  local run_id="$5"
  local fingerprint="$6"
  local run_dir="$7"

  python3 - "$manifest_file" "$kind" "$target_filter" "$profile_filter" "$run_id" "$fingerprint" "$run_dir" "$REPORT_ROOT" <<'PY'
import json
import os
import re
import stat
import sys

manifest_file, kind, target_filter, profile_filter, run_id, fingerprint, run_dir, report_root = sys.argv[1:]

def is_single_link_regular_file(path):
    try:
        metadata = os.lstat(path)
    except OSError:
        return False
    return stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1

def is_safe_component(value):
    return (
        isinstance(value, str)
        and value not in {".", ".."}
        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", value) is not None
    )

def is_safe_directory(path):
    try:
        metadata = os.lstat(path)
    except OSError:
        return False
    return stat.S_ISDIR(metadata.st_mode) and not stat.S_ISLNK(metadata.st_mode)

def has_expected_physical_layout(manifest):
    if kind not in {"micro", "qps", "rps"}:
        return False
    manifest_kind = manifest.get("kind")
    manifest_target = manifest.get("target")
    manifest_profile = manifest.get("profile")
    manifest_run_id = manifest.get("run_id")
    if (
        manifest_kind != kind
        or (kind != "rps" and not is_safe_component(manifest_target))
        or (kind == "rps" and not is_safe_component(manifest_profile))
        or not isinstance(manifest_run_id, str)
        or re.fullmatch(r"[0-9]{8}_[0-9]{6}", manifest_run_id) is None
    ):
        return False
    selector = manifest_profile if kind == "rps" else manifest_target
    root = os.path.abspath(report_root)
    expected_run_dir = os.path.join(root, kind, selector, manifest_run_id)
    expected_manifest = os.path.join(expected_run_dir, "manifest.json")
    if (
        os.path.abspath(run_dir) != expected_run_dir
        or os.path.abspath(manifest_file) != expected_manifest
    ):
        return False
    components = (
        os.path.dirname(root),
        root,
        os.path.join(root, kind),
        os.path.join(root, kind, selector),
        expected_run_dir,
    )
    return all(is_safe_directory(component) for component in components)

try:
    if not is_single_link_regular_file(manifest_file):
        raise OSError("manifest is not a safe regular file")
    with open(manifest_file, encoding="utf-8") as stream:
        manifest = json.load(stream)
except (OSError, UnicodeError, json.JSONDecodeError):
    raise SystemExit(1)

if not isinstance(manifest, dict):
    raise SystemExit(1)

def is_local_regular_file(name, expected_name):
    if name != expected_name:
        return False
    root = os.path.abspath(run_dir)
    path = os.path.join(root, expected_name)
    return (
        is_single_link_regular_file(path)
        and os.path.abspath(path) == os.path.join(root, expected_name)
    )

def nonempty_string(value):
    return isinstance(value, str) and bool(value)

def strict_integer(value):
    return isinstance(value, int) and not isinstance(value, bool)

def nonnegative_integer(value):
    return strict_integer(value) and value >= 0

def positive_integer(value):
    return strict_integer(value) and value > 0

result = manifest.get("result")
if (
    not strict_integer(manifest.get("schema_version"))
    or manifest.get("schema_version") != 1
    or manifest.get("kind") != kind
    or not nonempty_string(manifest.get("target"))
    or not nonempty_string(manifest.get("profile"))
    or (target_filter != "*" and manifest.get("target") != target_filter)
    or (profile_filter != "*" and manifest.get("profile") != profile_filter)
    or manifest.get("run_id") != run_id
    or not re.fullmatch(r"[0-9]{8}_[0-9]{6}", run_id)
    or manifest.get("state") != "passed"
    or manifest.get("status") != "passed"
    or not nonempty_string(manifest.get("finished_at"))
    or not nonempty_string(manifest.get("started_at"))
    or not positive_integer(manifest.get("attempt"))
    or not nonempty_string(manifest.get("run_fingerprint"))
    or (fingerprint != "*" and manifest.get("run_fingerprint") != fingerprint)
    or not strict_integer(manifest.get("exit_code"))
    or manifest.get("exit_code") != 0
    or not nonnegative_integer(manifest.get("elapsed_seconds"))
    or not isinstance(manifest.get("fingerprint_inputs"), dict)
    or not isinstance(manifest.get("environment"), dict)
    or not isinstance(result, dict)
    or result.get("status") != "passed"
    or not strict_integer(result.get("exit_code"))
    or result.get("exit_code") != 0
    or not nonnegative_integer(result.get("elapsed_seconds"))
    or result.get("elapsed_seconds") != manifest.get("elapsed_seconds")
    or not isinstance(manifest.get("metrics"), dict)
    or not has_expected_physical_layout(manifest)
    or not is_local_regular_file(manifest.get("raw_file"), "raw.txt")
    or not is_local_regular_file(manifest.get("report_file"), "report.txt")
):
    raise SystemExit(1)
PY
}

manifest_is_resumable() {
  manifest_is_complete "$@"
}

write_manifest() {
  local run_dir="$1"
  local kind="$2"
  local target="$3"
  local profile="$4"
  local run_id="$5"
  local state="$6"
  local attempt="$7"
  local started_at="$8"
  local finished_at="$9"
  local fingerprint="${10}"
  local binary_sha256="${11}"
  local build_mode="${12}"
  local flags_sha256="${13}"
  local timeout_seconds="${14}"
  local exit_code="${15}"
  local elapsed_seconds="${16}"
  local status="${17}"
  local raw_file="${18}"
  local report_file="${19}"
  local temp_file

  report_run_dir_is_safe "$kind" "$target" "$profile" "$run_id" "$run_dir" || return 1
  temp_file=$(mktemp "$run_dir/.manifest.XXXXXX") || return 1
  if ! is_single_link_regular_file "$temp_file"; then
    red "错误: manifest 临时文件不是单链接普通文件: $temp_file"
    return 1
  fi
  if ! python3 - "$run_dir" "$kind" "$target" "$profile" "$run_id" "$state" "$attempt" \
    "$started_at" "$finished_at" "$fingerprint" "$binary_sha256" "$build_mode" "$flags_sha256" \
    "$timeout_seconds" "$exit_code" "$elapsed_seconds" "$status" "$raw_file" "$report_file" \
    "$GIT_HASH" "$GIT_DIRTY" "$BENCH_HOSTNAME" "$CPU_MODEL" "$CPU_CORES" "$MEMORY_KB" \
    > "$temp_file" <<'PY'
import json
import os
import stat
import sys

(
    run_dir,
    kind,
    target,
    profile,
    run_id,
    state,
    attempt,
    started_at,
    finished_at,
    fingerprint,
    binary_sha256,
    build_mode,
    flags_sha256,
    timeout_seconds,
    exit_code,
    elapsed_seconds,
    status,
    raw_file,
    report_file,
    git_hash,
    git_dirty,
    hostname,
    cpu_model,
    cpu_cores,
    memory_kb,
) = sys.argv[1:]

def optional_integer(value):
    return None if value == "" else int(value)

def integer_or_string(value):
    try:
        return int(value)
    except ValueError:
        return value

raw_path = os.path.join(run_dir, raw_file)
try:
    raw_metadata = os.lstat(raw_path)
    raw_output_bytes = (
        raw_metadata.st_size
        if state != "running" and stat.S_ISREG(raw_metadata.st_mode) and raw_metadata.st_nlink == 1
        else 0
    )
except OSError:
    raw_output_bytes = 0

fingerprint_inputs = {
    "binary_sha256": binary_sha256,
    "build_mode": build_mode,
    "git_dirty": git_dirty == "true",
    "git_hash": git_hash,
    "profile": profile,
    "target": target,
    "timeout_seconds": timeout_seconds,
}
if kind == "micro":
    fingerprint_inputs["bench_flags_sha256"] = flags_sha256

data = {
    "schema_version": 1,
    "kind": kind,
    "target": target,
    "profile": profile,
    "run_id": run_id,
    "state": state,
    "attempt": int(attempt),
    "started_at": started_at,
    "finished_at": finished_at or None,
    "run_fingerprint": fingerprint,
    "fingerprint_inputs": fingerprint_inputs,
    "environment": {
        "git_hash": git_hash,
        "git_dirty": git_dirty == "true",
        "hostname": hostname,
        "cpu_model": cpu_model,
        "cpu_cores": integer_or_string(cpu_cores),
        "memory_kb": integer_or_string(memory_kb),
    },
    "status": status,
    "exit_code": optional_integer(exit_code),
    "elapsed_seconds": optional_integer(elapsed_seconds),
    "raw_file": raw_file,
    "report_file": report_file,
    "result": {
        "status": status,
        "exit_code": optional_integer(exit_code),
        "elapsed_seconds": optional_integer(elapsed_seconds),
    },
    "metrics": {"raw_output_bytes": raw_output_bytes},
}
json.dump(data, sys.stdout, ensure_ascii=False, indent=2)
PY
  then
    if report_run_dir_is_safe "$kind" "$target" "$profile" "$run_id" "$run_dir"; then
      rm -f -- "$temp_file"
    fi
    return 1
  fi
  if ! report_run_dir_is_safe "$kind" "$target" "$profile" "$run_id" "$run_dir" || \
    ! is_single_link_regular_file "$temp_file"; then
    red "错误: manifest 发布前运行目录或临时文件不安全"
    return 1
  fi
  replace_artifact_file "$temp_file" "$run_dir/manifest.json" || return 1
  if ! report_run_dir_is_safe "$kind" "$target" "$profile" "$run_id" "$run_dir" || \
    ! is_single_link_regular_file "$run_dir/manifest.json"; then
    red "错误: manifest 发布后运行目录或文件不安全: $run_dir/manifest.json"
    return 1
  fi
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
  require_tools xmake python3 find sort sha256sum stat
  parse_run_options "$@" || return $?

  local bench_flags_value="${BENCH_FLAGS:---benchmark_min_time=0.1s}"
  if ! validate_bench_flags "$bench_flags_value"; then
    return 2
  fi

  if [ "$BENCHMARK_BUILD_MODE" = "debug" ]; then
    cmd_build --debug
  else
    cmd_build
  fi

  local -a bench_bins=()
  mapfile -t bench_bins < <(find "$PROJECT_ROOT/bin" -maxdepth 1 -type f -name 'bench_*' -printf '%p\n' | sort)
  if [ "${#bench_bins[@]}" -eq 0 ]; then
    red "未找到微基准二进制（bin/bench_*）"
    return 1
  fi

  local -a bench_targets=()
  local bin
  for bin in "${bench_bins[@]}"; do
    bench_targets+=("$(basename "$bin")")
  done
  ensure_new_run_available "micro" "$BENCHMARK_RUN_ID" "${bench_targets[@]}" || return 1

  local bench_flags_sha256
  bench_flags_sha256=$(safe_value_sha256 "$bench_flags_value")
  local bench_profile="${BENCH_PROFILE:-default}"
  local -a bench_flags=()
  read -r -a bench_flags <<< "$bench_flags_value"
  local failures=0
  local name run_dir manifest_file raw_file report_file raw_temp_file report_temp_file binary_sha256 fingerprint attempt
  local started_at finished_at elapsed rc status
  for bin in "${bench_bins[@]}"; do
    name=$(basename "$bin")
    binary_sha256=$(sha256sum "$bin")
    binary_sha256="${binary_sha256%% *}"
    fingerprint=$(make_run_fingerprint \
      kind micro target "$name" binary_sha256 "$binary_sha256" git_hash "$GIT_HASH" \
      git_dirty "$GIT_DIRTY" build_mode "$BENCHMARK_BUILD_MODE" bench_flags "$bench_flags_value" \
      profile "$bench_profile" timeout_seconds none)
    run_dir="$REPORT_ROOT/micro/$name/$BENCHMARK_RUN_ID"
    manifest_file="$run_dir/manifest.json"

    if [ "$BENCHMARK_RESUME" -eq 1 ] && ! resume_path_is_safe micro "$name" "$BENCHMARK_RUN_ID"; then
      return 1
    fi

    if [ "$BENCHMARK_RESUME" -eq 1 ] && \
      manifest_is_resumable "$manifest_file" micro "$name" "$bench_profile" "$BENCHMARK_RUN_ID" \
        "$fingerprint" "$run_dir"; then
      green "跳过已通过微基准: $name (run-id=$BENCHMARK_RUN_ID)"
      continue
    fi

    run_dir=$(prepare_target_run_dir micro "$name" "$BENCHMARK_RUN_ID") || return 1
    manifest_file="$run_dir/manifest.json"
    raw_file="$run_dir/raw.txt"
    report_file="$run_dir/report.txt"
    attempt=$(next_manifest_attempt "$manifest_file" micro "$name" "$bench_profile" "$BENCHMARK_RUN_ID" "$run_dir")
    started_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    write_manifest "$run_dir" micro "$name" "$bench_profile" "$BENCHMARK_RUN_ID" running "$attempt" \
      "$started_at" "" "$fingerprint" "$binary_sha256" "$BENCHMARK_BUILD_MODE" "$bench_flags_sha256" \
      none "" "" running raw.txt report.txt || return 1

    yellow "运行微基准: $name"
    raw_temp_file=$(create_artifact_temp_file micro "$name" "$BENCHMARK_RUN_ID" "$run_dir" raw.txt) || return 1
    local started_seconds
    started_seconds=$(date +%s)
    set +e
    "$bin" "${bench_flags[@]}" > "$raw_temp_file" 2>&1
    rc=$?
    set -e
    elapsed=$(( $(date +%s) - started_seconds ))
    finished_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    if [ "$rc" -eq 0 ]; then
      status="passed"
    else
      status="failed"
      failures=$((failures + 1))
    fi
    report_temp_file=$(create_artifact_temp_file micro "$name" "$BENCHMARK_RUN_ID" "$run_dir" report.txt) || return 1
    if ! {
      write_report_header "微基准测试报告" "$bench_profile"
      printf '目标: %s\n运行 ID: %s\n状态: %s\n退出码: %s\n耗时: %ss\n' \
        "$name" "$BENCHMARK_RUN_ID" "$status" "$rc" "$elapsed"
      printf '\n--- 原始输出 ---\n'
      cat "$raw_temp_file"
    } > "$report_temp_file"; then
      red "错误: 无法写入微基准临时报告: $report_temp_file"
      return 1
    fi
    cat "$raw_temp_file"
    publish_artifact_file micro "$name" "$BENCHMARK_RUN_ID" "$run_dir" raw.txt "$raw_temp_file" || return 1
    publish_artifact_file micro "$name" "$BENCHMARK_RUN_ID" "$run_dir" report.txt "$report_temp_file" || return 1
    write_manifest "$run_dir" micro "$name" "$bench_profile" "$BENCHMARK_RUN_ID" "$status" "$attempt" \
      "$started_at" "$finished_at" "$fingerprint" "$binary_sha256" "$BENCHMARK_BUILD_MODE" "$bench_flags_sha256" \
      none "$rc" "$elapsed" "$status" raw.txt report.txt || return 1
    green "微基准报告: $report_file"
  done

  if [ "$failures" -ne 0 ]; then
    red "微基准失败目标数: $failures"
    return 1
  fi
  green "微基准全部 ${#bench_bins[@]} 个目标通过"
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

cmd_qps() {
  require_tools xmake timeout python3 find sort sha256sum stat
  local profile="${QPS_PROFILE:-smoke}"
  local profile_is_explicit=0
  local -a run_options=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      smoke|full)
        if [ "$profile_is_explicit" -eq 1 ]; then
          red "错误: QPS profile 不能重复指定"
          return 2
        fi
        profile="$1"
        profile_is_explicit=1
        ;;
      *) run_options+=("$1") ;;
    esac
    shift
  done
  case "$profile" in
    smoke|full) ;;
    *) red "错误: QPS_PROFILE 仅支持 smoke 或 full"; return 2 ;;
  esac
  parse_run_options "${run_options[@]}" || return $?

  discover_qps_targets
  if [ "${#QPS_EXPECTED_TARGETS[@]}" -eq 0 ]; then
    red "错误: benchmark/qps_*.cpp 为空"
    return 1
  fi
  ensure_new_run_available "qps" "$BENCHMARK_RUN_ID" "${QPS_EXPECTED_TARGETS[@]}" || return 1
  if [ "$BENCHMARK_RESUME" -eq 1 ]; then
    local expected_target
    for expected_target in "${QPS_EXPECTED_TARGETS[@]}"; do
      resume_path_is_safe qps "$expected_target" "$BENCHMARK_RUN_ID" || return 1
    done
  fi
  if [ "$BENCHMARK_BUILD_MODE" = "debug" ]; then
    cmd_build --debug
  else
    cmd_build
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

  export QPS_PROFILE="$profile"
  local failures=0
  local target bin run_dir manifest_file raw_file report_file raw_temp_file report_temp_file binary_sha256 fingerprint attempt
  local rc status started_at finished_at started_seconds elapsed
  for target in "${QPS_EXPECTED_TARGETS[@]}"; do
    bin="$PROJECT_ROOT/bin/$target"
    binary_sha256=$(sha256sum "$bin")
    binary_sha256="${binary_sha256%% *}"
    fingerprint=$(make_run_fingerprint \
      kind qps target "$target" binary_sha256 "$binary_sha256" git_hash "$GIT_HASH" \
      git_dirty "$GIT_DIRTY" build_mode "$BENCHMARK_BUILD_MODE" profile "$profile" \
      timeout_seconds "$timeout_seconds")
    run_dir="$REPORT_ROOT/qps/$target/$BENCHMARK_RUN_ID"
    manifest_file="$run_dir/manifest.json"
    if [ "$BENCHMARK_RESUME" -eq 1 ] && \
      manifest_is_resumable "$manifest_file" qps "$target" "$profile" "$BENCHMARK_RUN_ID" \
        "$fingerprint" "$run_dir"; then
      green "跳过已通过 QPS: $target (run-id=$BENCHMARK_RUN_ID)"
      continue
    fi

    run_dir=$(prepare_target_run_dir qps "$target" "$BENCHMARK_RUN_ID") || return 1
    manifest_file="$run_dir/manifest.json"
    raw_file="$run_dir/raw.txt"
    report_file="$run_dir/report.txt"
    attempt=$(next_manifest_attempt "$manifest_file" qps "$target" "$profile" "$BENCHMARK_RUN_ID" "$run_dir")
    started_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    write_manifest "$run_dir" qps "$target" "$profile" "$BENCHMARK_RUN_ID" running "$attempt" \
      "$started_at" "" "$fingerprint" "$binary_sha256" "$BENCHMARK_BUILD_MODE" "" \
      "$timeout_seconds" "" "" running raw.txt report.txt || return 1

    yellow "运行 QPS: $target (profile=$profile, timeout=${timeout_seconds}s)"
    raw_temp_file=$(create_artifact_temp_file qps "$target" "$BENCHMARK_RUN_ID" "$run_dir" raw.txt) || return 1
    started_seconds=$(date +%s)
    set +e
    timeout --signal=TERM --kill-after=5s "${timeout_seconds}s" "$bin" > "$raw_temp_file" 2>&1
    rc=$?
    set -e
    elapsed=$(( $(date +%s) - started_seconds ))
    finished_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    if [ "$rc" -eq 0 ]; then
      status="passed"
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      status="timeout"
      failures=$((failures + 1))
    else
      status="failed"
      failures=$((failures + 1))
    fi
    report_temp_file=$(create_artifact_temp_file qps "$target" "$BENCHMARK_RUN_ID" "$run_dir" report.txt) || return 1
    if ! {
      write_report_header "QPS + 压力测试报告" "$profile"
      printf '目标: %s\n运行 ID: %s\n状态: %s\n退出码: %s\n耗时: %ss\n单目标超时: %ss\n' \
        "$target" "$BENCHMARK_RUN_ID" "$status" "$rc" "$elapsed" "$timeout_seconds"
      printf '\n--- 原始输出 ---\n'
      cat "$raw_temp_file"
    } > "$report_temp_file"; then
      red "错误: 无法写入 QPS 临时报告: $report_temp_file"
      return 1
    fi
    cat "$raw_temp_file"
    publish_artifact_file qps "$target" "$BENCHMARK_RUN_ID" "$run_dir" raw.txt "$raw_temp_file" || return 1
    publish_artifact_file qps "$target" "$BENCHMARK_RUN_ID" "$run_dir" report.txt "$report_temp_file" || return 1
    write_manifest "$run_dir" qps "$target" "$profile" "$BENCHMARK_RUN_ID" "$status" "$attempt" \
      "$started_at" "$finished_at" "$fingerprint" "$binary_sha256" "$BENCHMARK_BUILD_MODE" "" \
      "$timeout_seconds" "$rc" "$elapsed" "$status" raw.txt report.txt || return 1
    green "QPS 报告: $report_file"
  done

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
  -- 保留 MP3 文件头，唯一标记从签名探测区之后写入。
  local signature_prefix_size = 4
  assert(#marker <= #payload - signature_prefix_size, "unique request marker exceeds upload payload")
  local request_body = string.sub(payload, 1, signature_prefix_size) .. marker ..
    string.sub(payload, signature_prefix_size + #marker + 1)
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

rps_raw_dir_is_safe() {
  rps_run_dir_is_safe "$RPS_ACTIVE_PROFILE" "$BENCHMARK_RUN_ID" "$RPS_RUN_DIR" || return 1
  [ -d "$RPS_RAW_DIR" ] && [ ! -L "$RPS_RAW_DIR" ]
}

rps_create_raw_temp_file() {
  local cell_id="$1"
  local temp_file

  rps_raw_dir_is_safe || return 1
  temp_file=$(mktemp "$RPS_RAW_DIR/.${cell_id}.XXXXXX") || return 1
  if ! is_single_link_regular_file "$temp_file"; then
    rm -f -- "$temp_file"
    return 1
  fi
  printf '%s\n' "$temp_file"
}

rps_publish_raw_file() {
  local cell_id="$1"
  local temp_file="$2"
  local raw_file="$RPS_RAW_DIR/${cell_id}.txt"

  rps_raw_dir_is_safe || return 1
  is_single_link_regular_file "$temp_file" || return 1
  replace_artifact_file "$temp_file" "$raw_file" || return 1
  rps_raw_dir_is_safe && is_single_link_regular_file "$raw_file"
}

rps_make_cell_record() {
  python3 - "$@" <<'PY'
import json
import math
import sys

fields = (
    "index", "scenario", "method", "path", "payload", "concurrency", "duration_seconds", "repeat",
    "exit_code", "result", "error", "wrk_requests", "wrk_status_errors", "status_total", "status_2xx",
    "non_2xx", "unexpected_status", "socket_connect", "socket_read", "socket_write", "socket_timeout",
    "rps", "avg_latency", "p50", "p90", "p99", "status_codes", "raw_file",
)
integer_fields = {
    "index", "concurrency", "duration_seconds", "repeat", "exit_code", "wrk_requests", "wrk_status_errors",
    "status_total", "status_2xx", "non_2xx", "unexpected_status", "socket_connect", "socket_read",
    "socket_write", "socket_timeout",
}
if len(sys.argv) != len(fields) + 1:
    raise SystemExit(1)
record = dict(zip(fields, sys.argv[1:]))
for field in integer_fields:
    value = record[field]
    if not value.isdigit():
        raise SystemExit(1)
    record[field] = int(value)
try:
    rps = float(record["rps"])
except ValueError:
    raise SystemExit(1)
if not math.isfinite(rps) or rps < 0:
    raise SystemExit(1)
record["rps"] = rps
print(json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
PY
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
  local raw_file="$RPS_RAW_DIR/${cell_id}.txt"
  local raw_temp_file raw_rel="raw/${cell_id}.txt"
  create_wrk_lua "$lua_file" "$method" "$body_file" "$content_type" "$token" \
    "$range_header" "$expected_codes" "$disposition" "$unique_body" "$request_nonce" "$request_delay_ms"

  local threads="$concurrency"
  if [ "$threads" -gt "$CPU_CORES" ] 2>/dev/null; then
    threads="$CPU_CORES"
  fi
  local cell_timeout=$((duration + RPS_CELL_TIMEOUT_GRACE_SECONDS))
  local url="${RPS_BASE_URL}${path}"
  yellow "RPS: $scenario $method $path 并发=$concurrency 时长=${duration}s 重复=$repeat"
  RPS_ACTIVE_CELL_ID="$cell_id"
  RPS_ACTIVE_CELL_INDEX="$index"
  RPS_ACTIVE_CELL_SCENARIO="$scenario"
  RPS_ACTIVE_CELL_METHOD="$method"
  RPS_ACTIVE_CELL_PATH="$path"
  RPS_ACTIVE_CELL_PAYLOAD="$payload"
  RPS_ACTIVE_CELL_CONCURRENCY="$concurrency"
  RPS_ACTIVE_CELL_DURATION="$duration"
  RPS_ACTIVE_CELL_REPEAT="$repeat"
  rps_update_cell_manifest "$cell_id" running "" || return 1
  raw_temp_file=$(rps_create_raw_temp_file "$cell_id") || return 1
  set +e
  timeout --signal=TERM --kill-after=5s "${cell_timeout}s" \
    wrk -t"$threads" -c"$concurrency" -d"${duration}s" \
      --timeout "${RPS_REQUEST_TIMEOUT_SECONDS}s" --latency -s "$lua_file" "$url" \
      > "$raw_temp_file" 2>&1
  local rc=$?
  set -e
  cat "$raw_temp_file"

  local rps avg_latency p50 p90 p99 wrk_requests wrk_status_errors
  local status_total status_2xx non_2xx unexpected status_codes
  local socket_connect socket_read socket_write socket_timeout
  rps=$(raw_metric 'Requests/sec' 2 "$raw_temp_file" || true)
  avg_latency=$(raw_metric 'Latency' 2 "$raw_temp_file" || true)
  p50=$(raw_metric '^[[:space:]]+50%' 2 "$raw_temp_file" || true)
  p90=$(raw_metric '^[[:space:]]+90%' 2 "$raw_temp_file" || true)
  p99=$(raw_metric '^[[:space:]]+99%' 2 "$raw_temp_file" || true)
  wrk_requests=$(raw_metric '^HPS_WRK_REQUESTS ' 2 "$raw_temp_file" || true)
  wrk_status_errors=$(raw_metric '^HPS_WRK_STATUS_ERRORS ' 2 "$raw_temp_file" || true)
  status_total=$(raw_metric '^HPS_STATUS_TOTAL ' 2 "$raw_temp_file" || true)
  status_2xx=$(raw_metric '^HPS_STATUS_2XX ' 2 "$raw_temp_file" || true)
  non_2xx=$(raw_metric '^HPS_STATUS_NON_2XX ' 2 "$raw_temp_file" || true)
  unexpected=$(raw_metric '^HPS_STATUS_UNEXPECTED ' 2 "$raw_temp_file" || true)
  status_codes=$(raw_metric '^HPS_STATUS_CODES ' 2 "$raw_temp_file" || true)
  socket_connect=$(raw_metric '^HPS_SOCKET_CONNECT ' 2 "$raw_temp_file" || true)
  socket_read=$(raw_metric '^HPS_SOCKET_READ ' 2 "$raw_temp_file" || true)
  socket_write=$(raw_metric '^HPS_SOCKET_WRITE ' 2 "$raw_temp_file" || true)
  socket_timeout=$(raw_metric '^HPS_SOCKET_TIMEOUT ' 2 "$raw_temp_file" || true)
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
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
      result="timeout"
      error_reason="wrk_timeout_${rc}"
    else
      result="failed"
      error_reason="wrk_exit_${rc}"
    fi
  elif ! is_finite_positive_number "$rps"; then
    result="failed"
    error_reason="invalid_rps"
  elif [ "$metric_valid" -ne 1 ] || \
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
  if [ "$result" = "failed" ] || [ "$result" = "timeout" ]; then
    RPS_FAILED_CELLS=$((RPS_FAILED_CELLS + 1))
  elif [ "$result" = "overloaded" ]; then
    RPS_OVERLOADED_CELLS=$((RPS_OVERLOADED_CELLS + 1))
  fi
  RPS_COMPLETED_CELLS=$((RPS_COMPLETED_CELLS + 1))

  printf '结果: %s RPS=%s status=%s non-2xx=%s unexpected=%s socket=%s/%s/%s/%s\n' \
    "$result" "${rps:-N/A}" "$status_codes" "$non_2xx" "$unexpected" \
    "$socket_connect" "$socket_read" "$socket_write" "$socket_timeout"
  rps_publish_raw_file "$cell_id" "$raw_temp_file" || return 1
  local cell_record record_rps=0
  if is_finite_positive_number "$rps"; then
    record_rps="$rps"
  fi
  cell_record=$(rps_make_cell_record \
    "$index" "$scenario" "$method" "$path" "$payload" "$concurrency" "$duration" "$repeat" \
    "$rc" "$result" "$error_reason" "${wrk_requests:-0}" "${wrk_status_errors:-0}" \
    "${status_total:-0}" "${status_2xx:-0}" "${non_2xx:-0}" "${unexpected:-0}" \
    "${socket_connect:-0}" "${socket_read:-0}" "${socket_write:-0}" "${socket_timeout:-0}" "$record_rps" \
    "${avg_latency:-N/A}" "${p50:-N/A}" "${p90:-N/A}" "${p99:-N/A}" "$status_codes" "$raw_rel") || return 1
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$index" "$scenario" "$method" "$path" "$payload" "$concurrency" "$duration" "$repeat" \
    "$rc" "$result" "$error_reason" "${wrk_requests:-0}" "${wrk_status_errors:-0}" \
    "${status_total:-0}" "${status_2xx:-0}" "${non_2xx:-0}" "${unexpected:-0}" \
    "${socket_connect:-0}" "${socket_read:-0}" "${socket_write:-0}" "${socket_timeout:-0}" "$record_rps" \
    "${avg_latency:-N/A}" "${p50:-N/A}" "${p90:-N/A}" "${p99:-N/A}" "$status_codes" "$raw_rel" \
    >> "$RPS_RESULTS_FILE"
  rps_update_cell_manifest "$cell_id" "$result" "$rc" "$cell_record" || return 1
  RPS_ACTIVE_CELL_ID=""
}

write_rps_reports() {
  local profile="$1"
  local strict_errors="$2"
  local preflight_status="$3"
  local preflight_error="$4"
  local json_temp text_temp

  rps_prepare_artifact "$profile" "$RPS_RUN_DIR" report.json || return 1
  json_temp=$(mktemp "$RPS_RUN_DIR/.report.json.XXXXXX") || return 1
  if ! is_single_link_regular_file "$json_temp"; then
    rm -f -- "$json_temp"
    return 1
  fi
  if ! python3 - "$RPS_RESULTS_FILE" "$json_temp" "$profile" "$RPS_BASE_URL" \
    "$RPS_EXPECTED_CELLS" "$strict_errors" "$preflight_status" "$preflight_error" \
    "$RPS_UPLOAD_DURATION_SECONDS" "$RPS_UPLOAD_DELAY_MILLISECONDS" \
    "$TIMESTAMP" "$GIT_HASH" "$GIT_DIRTY" "$BENCH_HOSTNAME" "$CPU_MODEL" "$CPU_CORES" "$MEMORY_KB" <<'PY'
import csv
import json
import math
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
        raise ValueError("invalid RPS")
    if not math.isfinite(row["rps"]) or row["rps"] < 0:
        raise ValueError("invalid RPS")
socket_errors = sum(
    row["socket_connect"] + row["socket_read"] + row["socket_write"] + row["socket_timeout"]
    for row in rows
)
failed = sum(row["result"] in {"failed", "timeout"} for row in rows)
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
  then
    rm -f -- "$json_temp"
    return 1
  fi
  rps_publish_artifact "$profile" report.json "$json_temp" || return 1

  text_temp=$(mktemp "$RPS_RUN_DIR/.report.txt.XXXXXX") || return 1
  if ! is_single_link_regular_file "$text_temp"; then
    rm -f -- "$text_temp"
    return 1
  fi
  if ! {
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
  } > "$text_temp"; then
    rm -f -- "$text_temp"
    return 1
  fi
  rps_publish_artifact "$profile" report.txt "$text_temp"
}

rps_run_dir_is_safe() {
  local profile="$1"
  local run_id="$2"
  local run_dir="$3"
  local allow_missing="${4:-0}"
  report_run_dir_is_safe rps rps "$profile" "$run_id" "$run_dir" "$allow_missing"
}

rps_prepare_run_dir() {
  local profile="$1"
  local run_dir="$REPORT_ROOT/rps/$profile/$BENCHMARK_RUN_ID"

  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$run_dir" 1 || return 1
  if [ "$BENCHMARK_RESUME" -eq 1 ]; then
    mkdir -p "$run_dir"
  else
    mkdir -p "$(dirname "$run_dir")"
    mkdir "$run_dir" || {
      red "错误: 新 RPS 运行不能创建目录: $run_dir"
      return 1
    }
  fi
  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$run_dir" || return 1
  printf '%s\n' "$run_dir"
}

rps_prepare_artifact() {
  local profile="$1"
  local run_dir="$2"
  local artifact="$3"
  local temp_file

  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$run_dir" || return 1
  if [ -e "$run_dir/$artifact" ] && is_single_link_regular_file "$run_dir/$artifact"; then
    return 0
  fi
  temp_file=$(mktemp "$run_dir/.${artifact}.XXXXXX") || return 1
  : > "$temp_file"
  if ! is_single_link_regular_file "$temp_file"; then
    rm -f -- "$temp_file"
    return 1
  fi
  replace_artifact_file "$temp_file" "$run_dir/$artifact" || return 1
  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$run_dir" && \
    is_single_link_regular_file "$run_dir/$artifact"
}

rps_publish_artifact() {
  local profile="$1"
  local artifact="$2"
  local temp_file="$3"
  local artifact_path="$RPS_RUN_DIR/$artifact"

  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$RPS_RUN_DIR" || return 1
  is_single_link_regular_file "$temp_file" || return 1
  replace_artifact_file "$temp_file" "$artifact_path" || return 1
  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$RPS_RUN_DIR" && \
    is_single_link_regular_file "$artifact_path"
}

rps_manifest_matches_fingerprint() {
  local profile="$1"
  local run_dir="$2"
  local fingerprint="$3"
  local manifest_file="$run_dir/manifest.json"

  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$run_dir" || return 1
  python3 - "$manifest_file" "$profile" "$BENCHMARK_RUN_ID" "$fingerprint" <<'PY'
import json
import os
import stat
import sys

manifest_file, profile, run_id, fingerprint = sys.argv[1:]
try:
    metadata = os.lstat(manifest_file)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise ValueError
    with open(manifest_file, encoding="utf-8") as stream:
        manifest = json.load(stream)
    if not isinstance(manifest, dict):
        raise ValueError
    assert manifest.get("schema_version") == 1
    assert manifest.get("kind") == "rps"
    assert manifest.get("target") == "rps"
    assert manifest.get("profile") == profile
    assert manifest.get("run_id") == run_id
    assert manifest.get("run_fingerprint") == fingerprint
    assert isinstance(manifest.get("cells"), dict)
except (AssertionError, OSError, UnicodeError, ValueError, TypeError, json.JSONDecodeError):
    raise SystemExit(1)
PY
}

rps_write_manifest() {
  local run_dir="$1"
  local profile="$2"
  local fingerprint="$3"
  local fingerprint_inputs="$4"
  local state="$5"
  local attempt="$6"
  local started_at="$7"
  local finished_at="$8"
  local exit_code="$9"
  local expected_cells="${10}"
  local cell_id="${11}"
  local cell_state="${12}"
  local cell_exit_code="${13}"
  local reset_cells="${14}"
  local target_mode="${15}"
  local cell_record_json="${16:-}"
  local temp_file

  rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$run_dir" || return 1
  temp_file=$(mktemp "$run_dir/.manifest.XXXXXX") || return 1
  if ! is_single_link_regular_file "$temp_file"; then
    return 1
  fi
  if ! python3 - "$run_dir" "$profile" "$BENCHMARK_RUN_ID" "$fingerprint" "$fingerprint_inputs" \
    "$state" "$attempt" "$started_at" "$finished_at" "$exit_code" "$expected_cells" \
    "$cell_id" "$cell_state" "$cell_exit_code" "$reset_cells" "$target_mode" \
    "$cell_record_json" "$GIT_HASH" "$GIT_DIRTY" "$BENCH_HOSTNAME" "$CPU_MODEL" "$CPU_CORES" "$MEMORY_KB" \
    > "$temp_file" <<'PY'
import json
import os
import stat
import sys

(
    run_dir, profile, run_id, fingerprint, inputs_json, state, attempt, started_at,
    finished_at, exit_code, expected_cells, cell_id, cell_state, cell_exit_code,
    reset_cells, target_mode, cell_record_json, git_hash, git_dirty, hostname,
    cpu_model, cpu_cores, memory_kb,
) = sys.argv[1:]
manifest_file = os.path.join(run_dir, "manifest.json")

def safe_manifest(path):
    try:
        metadata = os.lstat(path)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            return None
        with open(path, encoding="utf-8") as stream:
            value = json.load(stream)
        return value if isinstance(value, dict) else None
    except (OSError, UnicodeError, ValueError, TypeError, json.JSONDecodeError):
        return None

old = safe_manifest(manifest_file)
if (
    reset_cells == "1" or not old or old.get("kind") != "rps" or old.get("target") != "rps"
    or old.get("profile") != profile or old.get("run_id") != run_id
    or old.get("run_fingerprint") != fingerprint or not isinstance(old.get("cells"), dict)
):
    cells = {}
else:
    cells = old["cells"]

if cell_id:
    previous = cells.get(cell_id) if isinstance(cells.get(cell_id), dict) else {}
    previous_attempt = previous.get("attempt") if type(previous.get("attempt")) is int else 0
    if cell_state == "running":
        cell_attempt = previous_attempt + 1
    elif previous.get("state") == "running":
        cell_attempt = previous_attempt
    else:
        cell_attempt = previous_attempt + 1
    cell = {
        "state": cell_state,
        "status": cell_state,
        "attempt": cell_attempt,
        "exit_code": None if cell_exit_code == "" else int(cell_exit_code),
        "started_at": started_at,
        "finished_at": None if cell_state == "running" else finished_at,
    }
    if cell_state != "running":
        try:
            record = json.loads(cell_record_json)
        except json.JSONDecodeError:
            raise SystemExit("invalid RPS cell record")
        if not isinstance(record, dict) or record.get("result") != cell_state or \
           record.get("exit_code") != cell["exit_code"]:
            raise SystemExit("invalid RPS cell record")
        cell["record"] = record
    cells[cell_id] = cell

try:
    fingerprint_inputs = json.loads(inputs_json)
except json.JSONDecodeError:
    raise SystemExit("invalid RPS fingerprint inputs")
if not isinstance(fingerprint_inputs, dict):
    raise SystemExit("invalid RPS fingerprint input object")

completed = sum(cell.get("state") in {"passed", "failed", "timeout", "overloaded"} for cell in cells.values() if isinstance(cell, dict))
failed = sum(cell.get("state") in {"failed", "timeout"} for cell in cells.values() if isinstance(cell, dict))
overloaded = sum(cell.get("state") == "overloaded" for cell in cells.values() if isinstance(cell, dict))

def optional_integer(value):
    return None if value == "" else int(value)

data = {
    "schema_version": 1,
    "kind": "rps",
    "target": "rps",
    "profile": profile,
    "run_id": run_id,
    "state": state,
    "attempt": int(attempt),
    "started_at": started_at,
    "finished_at": finished_at or None,
    "run_fingerprint": fingerprint,
    "fingerprint_inputs": fingerprint_inputs,
    "environment": {
        "target_mode": target_mode,
        "git_hash": git_hash,
        "git_dirty": git_dirty == "true",
        "hostname": hostname,
        "cpu_model": cpu_model,
        "cpu_cores": int(cpu_cores) if cpu_cores.isdigit() else cpu_cores,
        "memory_kb": int(memory_kb) if memory_kb.isdigit() else memory_kb,
    },
    "status": state,
    "exit_code": optional_integer(exit_code),
    "elapsed_seconds": 0 if state != "running" else None,
    "raw_file": "raw.txt",
    "report_file": "report.txt",
    "result": {
        "status": state,
        "exit_code": optional_integer(exit_code),
        "elapsed_seconds": 0 if state != "running" else None,
    },
    "metrics": {
        "expected_cells": int(expected_cells),
        "completed_cells": completed,
        "failed_cells": failed,
        "overloaded_cells": overloaded,
    },
    "cells": cells,
}
json.dump(data, sys.stdout, ensure_ascii=False, indent=2)
sys.stdout.write("\n")
PY
  then
    rm -f -- "$temp_file"
    return 1
  fi
  if ! rps_run_dir_is_safe "$profile" "$BENCHMARK_RUN_ID" "$run_dir" || \
    ! is_single_link_regular_file "$temp_file"; then
    return 1
  fi
  replace_artifact_file "$temp_file" "$run_dir/manifest.json"
}

rps_cell_is_complete() {
  local cell_id="$1"
  local allow_overloaded="$2"
  local manifest_file="$RPS_RUN_DIR/manifest.json"

rps_run_dir_is_safe "$RPS_ACTIVE_PROFILE" "$BENCHMARK_RUN_ID" "$RPS_RUN_DIR" || return 1
  python3 - "$manifest_file" "$RPS_ACTIVE_PROFILE" "$BENCHMARK_RUN_ID" "$RPS_RUN_FINGERPRINT" \
    "$cell_id" "$allow_overloaded" "$RPS_RUN_DIR" <<'PY'
import json
import math
import os
import stat
import sys

manifest_file, profile, run_id, fingerprint, cell_id, allow_overloaded, run_dir = sys.argv[1:]
def regular(path):
    try:
        value = os.lstat(path)
        return stat.S_ISREG(value.st_mode) and value.st_nlink == 1
    except OSError:
        return False

def complete_cell(cell, expected_id):
    integer_fields = {
        "index", "concurrency", "duration_seconds", "repeat", "exit_code", "wrk_requests",
        "wrk_status_errors", "status_total", "status_2xx", "non_2xx", "unexpected_status",
        "socket_connect", "socket_read", "socket_write", "socket_timeout",
    }
    string_fields = {
        "scenario", "method", "path", "payload", "result", "error", "avg_latency", "p50",
        "p90", "p99", "status_codes", "raw_file",
    }
    required = integer_fields | string_fields | {"rps"}
    if set(cell) != {"state", "status", "attempt", "exit_code", "started_at", "finished_at", "record"}:
        return False
    state = cell.get("state")
    if state not in {"passed", "failed", "timeout", "overloaded"} or cell.get("status") != state:
        return False
    if type(cell.get("attempt")) is not int or cell["attempt"] < 1:
        return False
    if type(cell.get("exit_code")) is not int or cell["exit_code"] < 0:
        return False
    if state in {"passed", "overloaded"} and cell["exit_code"] != 0:
        return False
    if not all(isinstance(cell.get(field), str) and cell[field] for field in ("started_at", "finished_at")):
        return False
    record = cell.get("record")
    if not isinstance(record, dict) or set(record) != required:
        return False
    if record.get("result") != state or record.get("exit_code") != cell["exit_code"]:
        return False
    if any(type(record.get(field)) is not int or record[field] < 0 for field in integer_fields):
        return False
    if any(not isinstance(record.get(field), str) for field in string_fields):
        return False
    if type(record.get("rps")) not in {int, float} or isinstance(record.get("rps"), bool) or \
       not math.isfinite(record["rps"]) or record["rps"] < 0 or \
       (state in {"passed", "overloaded"} and record["rps"] <= 0):
        return False
    raw_dir = os.path.join(run_dir, "raw")
    try:
        raw_dir_info = os.lstat(raw_dir)
    except OSError:
        return False
    if not stat.S_ISDIR(raw_dir_info.st_mode) or stat.S_ISLNK(raw_dir_info.st_mode):
        return False
    raw_file = record["raw_file"]
    raw_path = os.path.join(run_dir, raw_file)
    return raw_file == f"raw/{expected_id}.txt" and regular(raw_path) and \
        os.path.abspath(raw_path) == os.path.join(os.path.abspath(run_dir), "raw", f"{expected_id}.txt")

try:
    if not regular(manifest_file) or not regular(os.path.join(run_dir, "raw.txt")) or \
       not regular(os.path.join(run_dir, "report.txt")) or not regular(os.path.join(run_dir, "report.json")):
        raise ValueError
    with open(manifest_file, encoding="utf-8") as stream:
        manifest = json.load(stream)
    assert isinstance(manifest, dict)
    assert manifest.get("schema_version") == 1
    assert manifest.get("kind") == "rps" and manifest.get("target") == "rps"
    assert manifest.get("profile") == profile and manifest.get("run_id") == run_id
    assert manifest.get("run_fingerprint") == fingerprint
    cell = manifest.get("cells", {}).get(cell_id)
    assert isinstance(manifest.get("cells"), dict) and isinstance(cell, dict)
    assert complete_cell(cell, cell_id)
    state = cell.get("state")
    assert state == "passed" or (allow_overloaded == "1" and state == "overloaded")
except (AssertionError, OSError, UnicodeError, ValueError, TypeError, json.JSONDecodeError):
    raise SystemExit(1)
PY
}

rps_prune_manifest_cells_to_expected() {
  local manifest_file="$RPS_RUN_DIR/manifest.json"
  local temp_file

  [ -n "${RPS_EXPECTED_CELLS_FILE:-}" ] && [ -f "$RPS_EXPECTED_CELLS_FILE" ] || return 1
  rps_run_dir_is_safe "$RPS_ACTIVE_PROFILE" "$BENCHMARK_RUN_ID" "$RPS_RUN_DIR" || return 1
  rps_manifest_matches_fingerprint "$RPS_ACTIVE_PROFILE" "$RPS_RUN_DIR" "$RPS_RUN_FINGERPRINT" || return 1
  temp_file=$(mktemp "$RPS_RUN_DIR/.manifest.XXXXXX") || return 1
  if ! is_single_link_regular_file "$temp_file"; then
    rm -f -- "$temp_file"
    return 1
  fi
  if ! python3 - "$manifest_file" "$RPS_EXPECTED_CELLS_FILE" > "$temp_file" <<'PY'
import json
import os
import stat
import sys

manifest_file, expected_file = sys.argv[1:]

def regular(path):
    try:
        value = os.lstat(path)
        return stat.S_ISREG(value.st_mode) and value.st_nlink == 1
    except OSError:
        return False

if not regular(manifest_file) or not regular(expected_file):
    raise SystemExit(1)
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)
with open(expected_file, encoding="utf-8") as stream:
    expected = [line.rstrip("\n") for line in stream if line.rstrip("\n")]
if not isinstance(manifest, dict) or not isinstance(manifest.get("cells"), dict):
    raise SystemExit(1)
if not expected or len(expected) != len(set(expected)):
    raise SystemExit(1)
cells = manifest["cells"]
manifest["cells"] = {cell_id: cells[cell_id] for cell_id in expected if cell_id in cells}
json.dump(manifest, sys.stdout, ensure_ascii=False, indent=2)
sys.stdout.write("\n")
PY
  then
    rm -f -- "$temp_file"
    return 1
  fi
  if ! rps_run_dir_is_safe "$RPS_ACTIVE_PROFILE" "$BENCHMARK_RUN_ID" "$RPS_RUN_DIR" || \
     ! is_single_link_regular_file "$temp_file"; then
    rm -f -- "$temp_file"
    return 1
  fi
  replace_artifact_file "$temp_file" "$manifest_file"
}

rps_rebuild_results_from_manifest() {
  local rebuilt_file

  [ -n "${RPS_EXPECTED_CELLS_FILE:-}" ] && [ -f "$RPS_EXPECTED_CELLS_FILE" ] || return 1
  rps_prune_manifest_cells_to_expected || return 1
  rebuilt_file=$(mktemp "$RPS_TMP_DIR/.results.XXXXXX") || return 1
  if ! python3 - "$RPS_RUN_DIR/manifest.json" "$RPS_EXPECTED_CELLS_FILE" "$RPS_RUN_DIR" \
    > "$rebuilt_file" <<'PY'
import csv
import json
import math
import os
import stat
import sys

manifest_file, expected_file, run_dir = sys.argv[1:]
fields = (
    "index", "scenario", "method", "path", "payload", "concurrency", "duration_seconds", "repeat",
    "exit_code", "result", "error", "wrk_requests", "wrk_status_errors", "status_total", "status_2xx",
    "non_2xx", "unexpected_status", "socket_connect", "socket_read", "socket_write", "socket_timeout",
    "rps", "avg_latency", "p50", "p90", "p99", "status_codes", "raw_file",
)
integer_fields = {
    "index", "concurrency", "duration_seconds", "repeat", "exit_code", "wrk_requests", "wrk_status_errors",
    "status_total", "status_2xx", "non_2xx", "unexpected_status", "socket_connect", "socket_read",
    "socket_write", "socket_timeout",
}
string_fields = set(fields) - integer_fields - {"rps"}

def regular(path):
    try:
        value = os.lstat(path)
        return stat.S_ISREG(value.st_mode) and value.st_nlink == 1
    except OSError:
        return False

def complete_cell(cell, expected_id, expected_index):
    if set(cell) != {"state", "status", "attempt", "exit_code", "started_at", "finished_at", "record"}:
        return None
    state = cell.get("state")
    if state not in {"passed", "failed", "timeout", "overloaded"} or cell.get("status") != state:
        return None
    if type(cell.get("attempt")) is not int or cell["attempt"] < 1:
        return None
    if type(cell.get("exit_code")) is not int or cell["exit_code"] < 0:
        return None
    if state in {"passed", "overloaded"} and cell["exit_code"] != 0:
        return None
    if not all(isinstance(cell.get(field), str) and cell[field] for field in ("started_at", "finished_at")):
        return None
    record = cell.get("record")
    if not isinstance(record, dict) or set(record) != set(fields):
        return None
    if record.get("result") != state or record.get("exit_code") != cell["exit_code"]:
        return None
    if record.get("index") != expected_index:
        return None
    if any(type(record.get(field)) is not int or record[field] < 0 for field in integer_fields):
        return None
    if any(not isinstance(record.get(field), str) for field in string_fields):
        return None
    if type(record.get("rps")) not in {int, float} or isinstance(record.get("rps"), bool) or \
       not math.isfinite(record["rps"]) or record["rps"] < 0 or \
       (state in {"passed", "overloaded"} and record["rps"] <= 0):
        return None
    raw_dir = os.path.join(run_dir, "raw")
    try:
        raw_dir_info = os.lstat(raw_dir)
    except OSError:
        return None
    if not stat.S_ISDIR(raw_dir_info.st_mode) or stat.S_ISLNK(raw_dir_info.st_mode):
        return None
    raw_file = record["raw_file"]
    raw_path = os.path.join(run_dir, raw_file)
    if raw_file != f"raw/{expected_id}.txt" or not regular(raw_path):
        return None
    if os.path.abspath(raw_path) != os.path.join(os.path.abspath(run_dir), "raw", f"{expected_id}.txt"):
        return None
    return record

try:
    if not regular(manifest_file):
        raise ValueError
    with open(manifest_file, encoding="utf-8") as stream:
        manifest = json.load(stream)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("cells"), dict):
        raise ValueError
    with open(expected_file, encoding="utf-8") as stream:
        expected = [line.rstrip("\n") for line in stream if line.rstrip("\n")]
    if not expected or len(expected) != len(set(expected)) or set(manifest["cells"]) != set(expected):
        raise ValueError
    rows = []
    for index, cell_id in enumerate(expected, start=1):
        cell = manifest["cells"].get(cell_id)
        if not isinstance(cell, dict):
            raise ValueError
        record = complete_cell(cell, cell_id, index)
        if record is None:
            raise ValueError
        rows.append(record)
except (OSError, UnicodeError, ValueError, TypeError, json.JSONDecodeError):
    raise SystemExit(1)

writer = csv.DictWriter(sys.stdout, fieldnames=fields, delimiter="\t", lineterminator="\n")
writer.writeheader()
writer.writerows(rows)
PY
  then
    rm -f -- "$rebuilt_file"
    return 1
  fi
  mv -f -- "$rebuilt_file" "$RPS_RESULTS_FILE"
}

rps_update_cell_manifest() {
  local cell_id="$1"
  local cell_state="$2"
  local cell_exit_code="$3"
  local cell_record_json="${4:-}"
  local finished_at=""
  [ "$cell_state" = running ] || finished_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  rps_write_manifest "$RPS_RUN_DIR" "$RPS_ACTIVE_PROFILE" "$RPS_RUN_FINGERPRINT" \
    "$RPS_FINGERPRINT_INPUTS" running "$RPS_MANIFEST_ATTEMPT" "$RPS_PROFILE_STARTED_AT" \
    "$finished_at" "" "$RPS_EXPECTED_CELLS" "$cell_id" "$cell_state" "$cell_exit_code" \
    0 "$RPS_TARGET_MODE" "$cell_record_json"
}

rps_finish_profile_manifest() {
  local state="$1"
  local exit_code="$2"
  rps_write_manifest "$RPS_RUN_DIR" "$RPS_ACTIVE_PROFILE" "$RPS_RUN_FINGERPRINT" \
    "$RPS_FINGERPRINT_INPUTS" "$state" "$RPS_MANIFEST_ATTEMPT" "$RPS_PROFILE_STARTED_AT" \
    "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$exit_code" "$RPS_EXPECTED_CELLS" "" "" "" \
    0 "$RPS_TARGET_MODE"
}

cleanup_rps_tmp() {
  if [[ -n "${RPS_TMP_DIR:-}" && -d "${RPS_TMP_DIR:-}" && "${RPS_TMP_DIR:-}" == /tmp/hps_rps_* ]]; then
    rm -rf -- "$RPS_TMP_DIR"
  fi
  RPS_TMP_DIR=""
}

rps_preflight_failure() {
  local message="$1"
  local exit_code="${2:-1}"
  red "$message"
  write_rps_reports "$RPS_ACTIVE_PROFILE" "$RPS_STRICT_ERRORS" failed "$message" || true
  rps_finish_profile_manifest failed "$exit_code" || true
  cleanup_rps_tmp
  return "$exit_code"
}

rps_mark_profile_started() {
  local profile="$1"
  local known

  for known in "${RPS_STARTED_PROFILES[@]}"; do
    [ "$known" != "$profile" ] || return 0
  done
  RPS_STARTED_PROFILES+=("$profile")
}

rps_profile_has_started() {
  local profile="$1"
  local known

  for known in "${RPS_STARTED_PROFILES[@]}"; do
    [ "$known" != "$profile" ] || return 0
  done
  return 1
}

rps_finalize_active_cell_for_signal() {
  local signal_rc="$1"
  local cell_record cell_id

  cell_id="${RPS_ACTIVE_CELL_ID:-}"
  [ -n "$cell_id" ] || return 0
  [ -n "${RPS_RESULTS_FILE:-}" ] && [ -f "$RPS_RESULTS_FILE" ] || return 1
  cell_record=$(rps_make_cell_record \
    "$RPS_ACTIVE_CELL_INDEX" "$RPS_ACTIVE_CELL_SCENARIO" "$RPS_ACTIVE_CELL_METHOD" \
    "$RPS_ACTIVE_CELL_PATH" "$RPS_ACTIVE_CELL_PAYLOAD" "$RPS_ACTIVE_CELL_CONCURRENCY" \
    "$RPS_ACTIVE_CELL_DURATION" "$RPS_ACTIVE_CELL_REPEAT" "$signal_rc" failed signal_interrupted \
    0 0 0 0 0 0 0 0 0 0 0 N/A N/A N/A N/A - "raw/${cell_id}.txt") || return 1
  rps_update_cell_manifest "$cell_id" failed "$signal_rc" "$cell_record" || return 1
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$RPS_ACTIVE_CELL_INDEX" "$RPS_ACTIVE_CELL_SCENARIO" "$RPS_ACTIVE_CELL_METHOD" \
    "$RPS_ACTIVE_CELL_PATH" "$RPS_ACTIVE_CELL_PAYLOAD" "$RPS_ACTIVE_CELL_CONCURRENCY" \
    "$RPS_ACTIVE_CELL_DURATION" "$RPS_ACTIVE_CELL_REPEAT" "$signal_rc" failed signal_interrupted \
    0 0 0 0 0 0 0 0 0 0 0 N/A N/A N/A N/A - "raw/${cell_id}.txt" >> "$RPS_RESULTS_FILE"
  RPS_ACTIVE_CELL_ID=""
}

rps_read_isolated_env_value() {
  local key="$1"
  local env_file="${HPS_ISOLATED_ENV_FILE:-}"
  [ -n "$env_file" ] && [ -f "$env_file" ] && [ ! -L "$env_file" ] || return 1
  awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$env_file"
}

rps_auth_payload() {
  local username="$1"
  local password="$2"
  local email="${3:-}"

  printf '%s\0%s\0%s\0' "$username" "$password" "$email" | python3 -c '
import json
import sys

values = sys.stdin.buffer.read().split(b"\0")
if len(values) != 4 or values[-1]:
    raise SystemExit(1)
username, password, email = (value.decode("utf-8") for value in values[:3])
payload = {"username": username, "password": password}
if email:
    payload["email"] = email
print(json.dumps(payload))
'
}

rps_authenticate() {
  local username password endpoint expected_status payload response raw status rc token parse_rc
  if [ "$RPS_TARGET_MODE" = managed ]; then
    username="$(rps_read_isolated_env_value ADMIN_USERNAME)" || return 1
    password="$(rps_read_isolated_env_value ADMIN_PASSWORD)" || return 1
    endpoint=/api/auth/login
    expected_status=200
    payload=$(rps_auth_payload "$username" "$password") || return 1
  else
    local suffix="${TIMESTAMP}_$$_${RANDOM}"
    username="rps_${suffix}"
    password="Rps_${suffix}_Pass!"
    endpoint=/api/auth/register
    expected_status=201
    payload=$(rps_auth_payload "$username" "$password" "${username}@example.invalid") || return 1
  fi
  response="$RPS_TMP_DIR/auth.json"
  raw="$RPS_TMP_DIR/auth.stderr"
  set +e
  status=$(printf '%s' "$payload" | curl -sS --connect-timeout 5 --max-time 30 -o "$response" -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' --data-binary @- "$RPS_BASE_URL$endpoint" 2> "$raw")
  rc=$?
  set -e
  if [ "$rc" -ne 0 ] || [ "$status" != "$expected_status" ]; then
    return 1
  fi
  set +e
  token=$(python3 - "$response" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream).get("token", ""))
PY
)
  parse_rc=$?
  set -e
  # 服务 token 是标准 base64 的用户段（可带 `=` padding）加点号十六进制签名。
  [ "$parse_rc" -eq 0 ] &&
    [[ "$token" =~ ^[A-Za-z0-9+/]+={0,2}\.[A-Fa-f0-9]+$ ]] || return 1
  RPS_AUTH_TOKEN="$token"
}

rps_run_profile() {
  local profile="$1"
  local default_duration default_read default_transfer default_upload default_upload_duration default_upload_delay strict_errors
  case "$profile" in
    smoke) default_duration=5; default_read="1 10"; default_transfer="1 5"; default_upload=1; default_upload_duration=2; default_upload_delay=250; strict_errors=1 ;;
    full) default_duration=20; default_read="1 10 50 100 500 1000"; default_transfer="1 10 50 100"; default_upload="1 2"; default_upload_duration=5; default_upload_delay=250; strict_errors=1 ;;
    overload) default_duration=20; default_read="2000 5000 10000"; default_transfer="100 500 1000"; default_upload="2 5"; default_upload_duration=5; default_upload_delay=250; strict_errors=0 ;;
    *) red "错误: RPS profile 仅支持 smoke、full 或 overload"; return 2 ;;
  esac

  local duration="${RPS_DURATION_SECONDS:-$default_duration}"
  local repeats="${RPS_REPEATS:-1}"
  RPS_REQUEST_TIMEOUT_SECONDS="${RPS_REQUEST_TIMEOUT_SECONDS:-5}"
  RPS_CELL_TIMEOUT_GRACE_SECONDS="${RPS_CELL_TIMEOUT_GRACE_SECONDS:-30}"
  RPS_UPLOAD_DURATION_SECONDS="${RPS_UPLOAD_DURATION_SECONDS:-$default_upload_duration}"
  RPS_UPLOAD_DELAY_MILLISECONDS="${RPS_UPLOAD_DELAY_MILLISECONDS:-$default_upload_delay}"
  if ! is_positive_integer "$duration" || ! is_positive_integer "$repeats" || \
     ! is_positive_integer "$RPS_REQUEST_TIMEOUT_SECONDS" || ! is_positive_integer "$RPS_CELL_TIMEOUT_GRACE_SECONDS" || \
     ! is_positive_integer "$RPS_UPLOAD_DURATION_SECONDS" || ! is_nonnegative_integer "$RPS_UPLOAD_DELAY_MILLISECONDS"; then
    red "错误: RPS 时长、重复次数和超时必须为合法整数"
    return 2
  fi
  local -a read_concurrency=() transfer_concurrency=() upload_concurrency=()
  parse_integer_list "${RPS_READ_CONCURRENCY:-$default_read}" read_concurrency || return 2
  parse_integer_list "${RPS_TRANSFER_CONCURRENCY:-$default_transfer}" transfer_concurrency || return 2
  parse_integer_list "${RPS_UPLOAD_CONCURRENCY:-$default_upload}" upload_concurrency || return 2

  RPS_ACTIVE_PROFILE="$profile"
  RPS_STRICT_ERRORS="$strict_errors"
  RPS_EXPECTED_CELLS=$((repeats * (4 * ${#read_concurrency[@]} + 3 * ${#transfer_concurrency[@]} + 2 * ${#upload_concurrency[@]})))
  RPS_COMPLETED_CELLS=0
  RPS_FAILED_CELLS=0
  RPS_OVERLOADED_CELLS=0
  RPS_RUN_DIR=$(rps_prepare_run_dir "$profile") || return 1
  if [ -n "${RPS_TEST_PAUSE_AFTER_PREPARE_SECONDS:-}" ]; then
    is_positive_integer "$RPS_TEST_PAUSE_AFTER_PREPARE_SECONDS" || return 2
    sleep "$RPS_TEST_PAUSE_AFTER_PREPARE_SECONDS"
  fi
  RPS_TEXT_FILE="$RPS_RUN_DIR/report.txt"
  RPS_JSON_FILE="$RPS_RUN_DIR/report.json"
  RPS_RAW_DIR="$RPS_RUN_DIR/raw"
  if [ -L "$RPS_RAW_DIR" ]; then
    red "错误: RPS 原始输出目录不允许是符号链接"
    return 1
  fi
  mkdir -p "$RPS_RAW_DIR"
  [ -d "$RPS_RAW_DIR" ] && [ ! -L "$RPS_RAW_DIR" ] || return 1
  rps_prepare_artifact "$profile" "$RPS_RUN_DIR" raw.txt || return 1
  rps_prepare_artifact "$profile" "$RPS_RUN_DIR" report.txt || return 1
  rps_prepare_artifact "$profile" "$RPS_RUN_DIR" report.json || return 1
  RPS_TMP_DIR=$(mktemp -d "/tmp/hps_rps_${BENCHMARK_RUN_ID}_${profile}.XXXXXX") || return 1
  RPS_RESULTS_FILE="$RPS_TMP_DIR/results.tsv"
  RPS_EXPECTED_CELLS_FILE="$RPS_TMP_DIR/expected-cells.txt"
  printf 'index\tscenario\tmethod\tpath\tpayload\tconcurrency\tduration_seconds\trepeat\texit_code\tresult\terror\twrk_requests\twrk_status_errors\tstatus_total\tstatus_2xx\tnon_2xx\tunexpected_status\tsocket_connect\tsocket_read\tsocket_write\tsocket_timeout\trps\tavg_latency\tp50\tp90\tp99\tstatus_codes\traw_file\n' > "$RPS_RESULTS_FILE"
  : > "$RPS_EXPECTED_CELLS_FILE"
  RPS_FINGERPRINT_INPUTS=$(python3 - "$profile" "$RPS_TARGET_FINGERPRINT_VALUE" "$duration" "$repeats" \
    "${read_concurrency[*]}" "${transfer_concurrency[*]}" "${upload_concurrency[*]}" \
    "$RPS_REQUEST_TIMEOUT_SECONDS" "$RPS_CELL_TIMEOUT_GRACE_SECONDS" "$RPS_UPLOAD_DURATION_SECONDS" "$RPS_UPLOAD_DELAY_MILLISECONDS" <<'PY'
import json
import sys
keys = ("profile", "target_fingerprint", "duration_seconds", "repeats", "read_concurrency", "transfer_concurrency", "upload_concurrency", "request_timeout_seconds", "cell_timeout_grace_seconds", "upload_duration_seconds", "upload_delay_milliseconds")
print(json.dumps(dict(zip(keys, sys.argv[1:])), ensure_ascii=False, sort_keys=True, separators=(",", ":")))
PY
)
  RPS_RUN_FINGERPRINT=$(make_run_fingerprint kind rps profile "$profile" target_fingerprint "$RPS_TARGET_FINGERPRINT_VALUE" \
    duration_seconds "$duration" repeats "$repeats" read_concurrency "${read_concurrency[*]}" \
    transfer_concurrency "${transfer_concurrency[*]}" upload_concurrency "${upload_concurrency[*]}" \
    request_timeout_seconds "$RPS_REQUEST_TIMEOUT_SECONDS" cell_timeout_grace_seconds "$RPS_CELL_TIMEOUT_GRACE_SECONDS" \
    upload_duration_seconds "$RPS_UPLOAD_DURATION_SECONDS" upload_delay_milliseconds "$RPS_UPLOAD_DELAY_MILLISECONDS")
  local reset_cells=1
  if rps_manifest_matches_fingerprint "$profile" "$RPS_RUN_DIR" "$RPS_RUN_FINGERPRINT"; then
    reset_cells=0
  fi
  RPS_MANIFEST_ATTEMPT=$(next_manifest_attempt "$RPS_RUN_DIR/manifest.json" rps rps "$profile" "$BENCHMARK_RUN_ID" "$RPS_RUN_DIR")
  RPS_PROFILE_STARTED_AT=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
  rps_write_manifest "$RPS_RUN_DIR" "$profile" "$RPS_RUN_FINGERPRINT" "$RPS_FINGERPRINT_INPUTS" running \
    "$RPS_MANIFEST_ATTEMPT" "$RPS_PROFILE_STARTED_AT" "" "" "$RPS_EXPECTED_CELLS" "" "" "" "$reset_cells" "$RPS_TARGET_MODE" || return 1
  RPS_PROFILE_READY=1
  rps_mark_profile_started "$profile"

  if [ -n "${RPS_STARTUP_FAILURE_MESSAGE:-}" ]; then
    rps_preflight_failure "$RPS_STARTUP_FAILURE_MESSAGE" "${RPS_STARTUP_FAILURE_EXIT_CODE:-1}" || true
    return "${RPS_STARTUP_FAILURE_EXIT_CODE:-1}"
  fi

  blue "=== RPS 预检: $RPS_BASE_URL ==="
  local health_body="$RPS_TMP_DIR/health.json" health_raw="$RPS_TMP_DIR/health.stderr" health_status health_rc
  set +e
  health_status=$(curl -sS --connect-timeout 5 --max-time 15 -o "$health_body" -w '%{http_code}' "$RPS_BASE_URL/api/health" 2> "$health_raw")
  health_rc=$?
  set -e
  if [ "$health_rc" -ne 0 ] || [ "$health_status" != 200 ]; then
    rps_preflight_failure "健康检查失败: curl=$health_rc HTTP=${health_status:-000}"
    return 1
  fi
  if ! rps_authenticate; then
    rps_preflight_failure 'RPS 认证预检失败'
    return 1
  fi

  local suffix="${TIMESTAMP}_$$_${RANDOM}"
  local payload_1kb="$RPS_TMP_DIR/payload_1kb.bin" payload_1mb="$RPS_TMP_DIR/payload_1mb.bin"
  dd if=/dev/urandom of="$payload_1kb" bs=1024 count=1 status=none
  dd if=/dev/urandom of="$payload_1mb" bs=1048576 count=1 status=none
  printf '\377\373\220\064' | dd of="$payload_1kb" bs=1 conv=notrunc status=none
  printf '\377\373\220\064' | dd of="$payload_1mb" bs=1 conv=notrunc status=none
  local -a seed_files=("$payload_1kb" "$payload_1mb") seed_labels=(1KB 1MB) seed_hashes=() seed_ids=()
  local seed_index seed_body seed_raw seed_status seed_rc seed_values seed_parse_rc file_hash file_id
  for seed_index in 0 1; do
    seed_body="$RPS_TMP_DIR/seed_${seed_index}.json"
    seed_raw="$RPS_TMP_DIR/seed_${seed_index}.stderr"
    set +e
    seed_status=$(curl -sS --connect-timeout 5 --max-time 120 -o "$seed_body" -w '%{http_code}' \
      -X POST -H 'Content-Type: audio/mpeg' \
      --header @<(printf 'Authorization: Bearer %s\n' "$RPS_AUTH_TOKEN") \
      -H "Content-Disposition: attachment; filename=\"rps_seed_${suffix}_${seed_labels[$seed_index]}.mp3\"" \
      --data-binary "@${seed_files[$seed_index]}" "$RPS_BASE_URL/api/files/upload" 2> "$seed_raw")
    seed_rc=$?
    set -e
    if [ "$seed_rc" -ne 0 ] || { [ "$seed_status" != 200 ] && [ "$seed_status" != 201 ]; }; then
      rps_preflight_failure "预上传 ${seed_labels[$seed_index]} 失败: curl=$seed_rc HTTP=${seed_status:-000}"
      return 1
    fi
    set +e
    seed_values=$(python3 - "$seed_body" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
print(value.get("file_hash", ""), value.get("file_id", ""))
PY
)
    seed_parse_rc=$?
    set -e
    if [ "$seed_parse_rc" -ne 0 ]; then
      rps_preflight_failure '预上传响应不是合法 JSON'
      return 1
    fi
    read -r file_hash file_id <<< "$seed_values"
    if [[ ! "$file_hash" =~ ^[A-Fa-f0-9]{64}$ ]] || [[ ! "$file_id" =~ ^[1-9][0-9]*$ ]]; then
      rps_preflight_failure '预上传响应缺少 file_hash/file_id'
      return 1
    fi
    seed_hashes+=("$file_hash")
    seed_ids+=("$file_id")
  done

  local -a scenario_names=(health auth_me files_list music_list upload_1kb upload_1mb download_1kb download_1mb range_1mb)
  local -a scenario_methods=(GET GET GET GET POST POST GET GET GET)
  local -a scenario_paths=("/api/health" "/api/auth/me" "/api/files?offset=0&limit=20" "/api/music/library?offset=0&limit=20" "/api/files/upload" "/api/files/upload" "/api/files/by-hash/${seed_hashes[0]}/download" "/api/files/by-hash/${seed_hashes[1]}/download" "/api/files/${seed_ids[1]}/stream")
  local -a scenario_payloads=(none none none none 1KB 1MB 1KB 1MB 1KB-range)
  local -a scenario_classes=(read read read read upload upload transfer transfer transfer)
  local -a scenario_bodies=("" "" "" "" "$payload_1kb" "$payload_1mb" "" "" "")
  local -a scenario_types=("" "" "" "" audio/mpeg audio/mpeg "" "" "")
  local -a scenario_tokens=("" "$RPS_AUTH_TOKEN" "$RPS_AUTH_TOKEN" "$RPS_AUTH_TOKEN" "$RPS_AUTH_TOKEN" "$RPS_AUTH_TOKEN" "$RPS_AUTH_TOKEN" "$RPS_AUTH_TOKEN" "$RPS_AUTH_TOKEN")
  local -a scenario_ranges=("" "" "" "" "" "" "" "" bytes=0-1023)
  local -a scenario_codes=(200 200 200 200 201 201 200 200 206)
  local -a scenario_unique_bodies=(0 0 0 0 1 1 0 0 0)
  local -a scenario_dispositions=("" "" "" "" "attachment; filename=\"rps_upload_${suffix}_1kb.mp3\"" "attachment; filename=\"rps_upload_${suffix}_1mb.mp3\"" "" "" "")

  blue "=== RPS 矩阵: profile=$profile cells=$RPS_EXPECTED_CELLS ==="
  local cell_index=0 scenario_index repeat concurrency cell_duration request_delay_ms cell_id
  local -a active_concurrency=()
  for repeat in $(seq 1 "$repeats"); do
    for scenario_index in "${!scenario_names[@]}"; do
      case "${scenario_classes[$scenario_index]}" in
        read) active_concurrency=("${read_concurrency[@]}"); cell_duration="$duration"; request_delay_ms=0 ;;
        transfer) active_concurrency=("${transfer_concurrency[@]}"); cell_duration="$duration"; request_delay_ms=0 ;;
        upload) active_concurrency=("${upload_concurrency[@]}"); cell_duration="$RPS_UPLOAD_DURATION_SECONDS"; request_delay_ms="$RPS_UPLOAD_DELAY_MILLISECONDS" ;;
      esac
      for concurrency in "${active_concurrency[@]}"; do
        cell_index=$((cell_index + 1))
        cell_id=$(printf '%03d_%s_c%s_r%s' "$cell_index" "${scenario_names[$scenario_index]}" "$concurrency" "$repeat")
        printf '%s\n' "$cell_id" >> "$RPS_EXPECTED_CELLS_FILE"
        if rps_cell_is_complete "$cell_id" "$((strict_errors == 0))"; then
          green "跳过已完成 RPS 单元: $cell_id"
          continue
        fi
        run_rps_cell "$cell_index" "${scenario_names[$scenario_index]}" "${scenario_methods[$scenario_index]}" \
          "${scenario_paths[$scenario_index]}" "${scenario_payloads[$scenario_index]}" "${scenario_bodies[$scenario_index]}" \
          "${scenario_types[$scenario_index]}" "${scenario_tokens[$scenario_index]}" "${scenario_ranges[$scenario_index]}" \
          "${scenario_codes[$scenario_index]}" "${scenario_dispositions[$scenario_index]}" "$concurrency" "$cell_duration" \
          "$repeat" "$strict_errors" "${scenario_unique_bodies[$scenario_index]}" "${suffix}_${cell_index}" "$request_delay_ms" || {
          rps_preflight_failure 'RPS 单元 manifest 更新失败'
          return 1
        }
        if [ "$profile" = overload ] && [ "${RPS_TARGET_MODE:-}" = managed ]; then
          hps_restart_isolated_environment || {
            rps_preflight_failure '过载单元后重启后端服务失败'
            return 1
          }
        fi
      done
    done
  done
  rps_rebuild_results_from_manifest || {
    rps_finish_profile_manifest failed 1 || true
    cleanup_rps_tmp
    return 1
  }
  write_rps_reports "$profile" "$strict_errors" passed - || {
    rps_finish_profile_manifest failed 1 || true
    cleanup_rps_tmp
    return 1
  }
  local result_state=passed result_code=0
  if [ "$RPS_FAILED_CELLS" -ne 0 ]; then result_state=failed; result_code=1; fi
  rps_finish_profile_manifest "$result_state" "$result_code" || return 1
  cleanup_rps_tmp
  RPS_PROFILE_READY=0
  green "文本报告: $RPS_TEXT_FILE"
  green "JSON 报告: $RPS_JSON_FILE"
  green "原始输出: $RPS_RAW_DIR"
  if [ "$result_state" = failed ]; then
    red "RPS 门禁失败: 失败 $RPS_FAILED_CELLS"
    return 1
  fi
  [ "$RPS_OVERLOADED_CELLS" -eq 0 ] || yellow "过载观测完成: $RPS_OVERLOADED_CELLS 个单元出现 HTTP/Socket 错误"
}

rps_finish_command() {
  local primary_rc="$1" cleanup_rc=0
  cleanup_rps_tmp
  if [ "${RPS_MANAGED_ENV:-0}" -eq 1 ] && [ "${RPS_MANAGED_CLEANED:-0}" -eq 0 ]; then
    RPS_MANAGED_CLEANED=1
    RPS_DEFER_MANAGED_CLEANUP=0
    if hps_cleanup_isolated_environment; then
      :
    else
      cleanup_rc=$?
      red "RPS 托管环境清理失败（project=${HPS_ISOLATED_PROJECT_NAME:-unknown}）"
    fi
  fi
  if [ "$primary_rc" -ne 0 ]; then return "$primary_rc"; fi
  return "$cleanup_rc"
}

rps_defer_managed_cleanup() {
  local cleanup_definition

  cleanup_definition="$(declare -f hps_cleanup_isolated_environment)" || return 1
  cleanup_definition="${cleanup_definition/hps_cleanup_isolated_environment/rps_original_hps_cleanup_isolated_environment}"
  eval "$cleanup_definition"
  hps_cleanup_isolated_environment() {
    if [ "${RPS_DEFER_MANAGED_CLEANUP:-0}" -eq 1 ]; then
      RPS_MANAGED_CLEANUP_DEFERRED=1
      return 0
    fi
    rps_original_hps_cleanup_isolated_environment
  }
  RPS_DEFER_MANAGED_CLEANUP=1
}

rps_write_startup_failure_reports() {
  local exit_code="$1"
  local message="$2"
  shift 2
  local profile previous_resume="$BENCHMARK_RESUME"
  local previous_prepare_pause="${RPS_TEST_PAUSE_AFTER_PREPARE_SECONDS:-}"

  RPS_STARTUP_FAILURE_MESSAGE="$message"
  RPS_STARTUP_FAILURE_EXIT_CODE="$exit_code"
  RPS_BASE_URL="http://managed-startup-failure.invalid"
  RPS_TARGET_FINGERPRINT_VALUE="managed-startup-failure"
  BENCHMARK_RESUME=1
  unset RPS_TEST_PAUSE_AFTER_PREPARE_SECONDS
  for profile in "$@"; do
    rps_run_profile "$profile" || true
  done
  BENCHMARK_RESUME="$previous_resume"
  if [ -n "$previous_prepare_pause" ]; then
    RPS_TEST_PAUSE_AFTER_PREPARE_SECONDS="$previous_prepare_pause"
  fi
  unset RPS_STARTUP_FAILURE_MESSAGE RPS_STARTUP_FAILURE_EXIT_CODE
}

rps_write_unstarted_profile_failure_reports() {
  local exit_code="$1"
  local message="$2"
  local profile
  local -a unstarted_profiles=()

  for profile in "${RPS_REQUESTED_PROFILES[@]}"; do
    if ! rps_profile_has_started "$profile"; then
      unstarted_profiles+=("$profile")
    fi
  done
  [ "${#unstarted_profiles[@]}" -eq 0 ] || \
    rps_write_startup_failure_reports "$exit_code" "$message" "${unstarted_profiles[@]}"
}

rps_signal_handler() {
  local signal_rc="$1"
  trap - INT TERM
  if [ "${RPS_PROFILE_READY:-0}" -eq 1 ] && [ -n "${RPS_ACTIVE_PROFILE:-}" ] && [ -n "${RPS_RUN_DIR:-}" ] && \
     [ -n "${RPS_RESULTS_FILE:-}" ] && [ -f "${RPS_RESULTS_FILE:-}" ]; then
    rps_finalize_active_cell_for_signal "$signal_rc" || true
    rps_preflight_failure "RPS 收到信号中断 (exit=$signal_rc)" "$signal_rc" || true
  fi
  rps_write_unstarted_profile_failure_reports "$signal_rc" "RPS 启动阶段收到信号中断 (exit=$signal_rc)" || true
  rps_finish_command "$signal_rc" || true
  exit "$signal_rc"
}

cmd_rps() {
  require_tools wrk curl python3 timeout dd awk stat
  local -a profiles=() run_options=()
  local argument profile seen
  while [ "$#" -gt 0 ]; do
    case "$1" in
      smoke|full|overload)
        for seen in "${profiles[@]}"; do
          [ "$seen" != "$1" ] || { red "错误: RPS profile 不能重复指定: $1"; return 2; }
        done
        profiles+=("$1")
        ;;
      *) run_options+=("$1") ;;
    esac
    shift
  done
  if [ "${#profiles[@]}" -eq 0 ]; then
    if [ -n "${RPS_PROFILE:-}" ]; then profiles=("$RPS_PROFILE"); else profiles=(full overload); fi
  fi
  for profile in "${profiles[@]}"; do
    case "$profile" in smoke|full|overload) ;; *) red "错误: RPS_PROFILE 仅支持 smoke、full 或 overload"; return 2 ;; esac
  done
  parse_run_options "${run_options[@]}" || return $?
  ensure_new_run_available rps "$BENCHMARK_RUN_ID" "${profiles[@]}" || return 1
  RPS_REQUESTED_PROFILES=("${profiles[@]}")
  RPS_STARTED_PROFILES=()
  RPS_PROFILE_READY=0
  RPS_ACTIVE_CELL_ID=""

  RPS_MANAGED_ENV=0
  RPS_MANAGED_CLEANED=0
  RPS_TARGET_MODE=external
  if [ -n "${RPS_BASE_URL:-}" ]; then
    RPS_BASE_URL="${RPS_BASE_URL%/}"
    if [ -z "${RPS_TARGET_FINGERPRINT:-}" ]; then
      red '错误: 显式 RPS_BASE_URL 必须同时设置非空 RPS_TARGET_FINGERPRINT'
      return 2
    fi
    RPS_TARGET_FINGERPRINT_VALUE="$RPS_TARGET_FINGERPRINT"
    trap 'rps_signal_handler 130' INT
    trap 'rps_signal_handler 143' TERM
  else
    source "$PROJECT_ROOT/scripts/lib/isolated_docker_env.sh"
    RPS_MANAGED_ENV=1
    RPS_TARGET_MODE=managed
    rps_defer_managed_cleanup || return 1
    trap 'rps_signal_handler 130' INT
    trap 'rps_signal_handler 143' TERM
    if hps_start_isolated_environment rps "$BENCHMARK_RUN_ID"; then
      :
    else
      local start_rc=$?
      rps_write_startup_failure_reports "$start_rc" "RPS 托管环境启动失败 (exit=$start_rc)" "${profiles[@]}" || true
      rps_finish_command "$start_rc" || true
      trap - INT TERM
      return "$start_rc"
    fi
    RPS_BASE_URL="$HPS_ISOLATED_BASE_URL"
    if RPS_TARGET_FINGERPRINT_VALUE="$(hps_runtime_fingerprint)"; then
      if [ -z "$RPS_TARGET_FINGERPRINT_VALUE" ]; then
        rps_write_startup_failure_reports 1 'RPS 托管目标指纹为空' "${profiles[@]}" || true
        rps_finish_command 1 || true
        trap - INT TERM
        return 1
      fi
    else
      local fingerprint_rc=$?
      rps_write_startup_failure_reports "$fingerprint_rc" \
        "RPS 托管目标指纹获取失败 (exit=$fingerprint_rc)" "${profiles[@]}" || true
      rps_finish_command "$fingerprint_rc" || true
      trap - INT TERM
      return "$fingerprint_rc"
    fi
  fi
  if [[ ! "$RPS_BASE_URL" =~ ^https?://[^/@?#[:space:]]+$ ]]; then
    red '错误: RPS_BASE_URL 必须是不含凭据、路径、查询或片段的 http(s) 入口'
    rps_finish_command 2 || true
    trap - INT TERM
    return 2
  fi

  local primary_rc=0 profile_rc
  for profile in "${profiles[@]}"; do
    if rps_run_profile "$profile"; then :; else
      profile_rc=$?
      [ "$primary_rc" -ne 0 ] || primary_rc="$profile_rc"
    fi
  done
  rps_finish_command "$primary_rc"
  local final_rc=$?
  trap - INT TERM
  return "$final_rc"
}

cmd_diff() {
  require_tools python3 find sort
  local kind="${1:-}"
  local target=""
  local profile=""
  [ "$kind" = "load" ] && kind="rps"
  case "$kind" in
    micro)
      if [ "$#" -ne 2 ]; then
        red "错误: 用法: diff micro <bench_target>"
        return 2
      fi
      target="$2"
      ;;
    qps)
      if [ "$#" -ne 3 ]; then
        red "错误: 用法: diff qps <qps_target> <profile>"
        return 2
      fi
      target="$2"
      profile="$3"
      ;;
    rps)
      if [ "$#" -ne 2 ]; then
        red "错误: 用法: diff rps <profile>"
        return 2
      fi
      profile="$2"
      ;;
    *)
      red "错误: diff 仅支持 micro、qps、rps（load 是 rps 的别名）"
      return 2
      ;;
  esac

  case "$kind" in
    micro)
      if ! is_safe_report_component "$target"; then
        red "错误: diff target 不是预期安全值"
        return 2
      fi
      ;;
    qps)
      if ! is_safe_report_component "$target" || ! is_safe_report_component "$profile"; then
        red "错误: diff target 或 profile 不是预期安全值"
        return 2
      fi
      ;;
    rps)
      if ! is_safe_report_component "$profile"; then
        red "错误: diff profile 不是预期安全值"
        return 2
      fi
      ;;
  esac

  if [ ! -d "$REPORT_ROOT" ]; then
    red "错误: 未找到新报告根: $REPORT_ROOT"
    return 1
  fi

  local target_filter="$target"
  local profile_filter="$profile"
  case "$kind" in
    micro) profile_filter="*" ;;
    rps) target_filter="*" ;;
  esac

  local -a candidates=()
  local manifest_file run_dir run_id
  while IFS= read -r manifest_file; do
    run_dir=$(dirname "$manifest_file")
    run_id=$(basename "$run_dir")
    if ! is_run_id "$run_id"; then
      continue
    fi
    if manifest_is_complete "$manifest_file" "$kind" "$target_filter" "$profile_filter" \
      "$run_id" "*" "$run_dir"; then
      candidates+=("${run_id}"$'\t'"${manifest_file}")
    fi
  done < <(
    find "$REPORT_ROOT" -type d -name '_legacy_aggregate' -prune -o -type f -name manifest.json -print | sort
  )

  if [ "${#candidates[@]}" -lt 2 ]; then
    red "错误: 至少需要两份完整 passed $kind manifest"
    return 1
  fi

  local -a sorted_candidates=()
  mapfile -t sorted_candidates < <(printf '%s\n' "${candidates[@]}" | sort)
  local previous_entry="${sorted_candidates[$((${#sorted_candidates[@]} - 2))]}"
  local current_entry="${sorted_candidates[$((${#sorted_candidates[@]} - 1))]}"
  local previous_file="${previous_entry#*$'\t'}"
  local current_file="${current_entry#*$'\t'}"

  python3 - "$previous_file" "$current_file" <<'PY'
import json
import sys

previous_file, current_file = sys.argv[1:]
with open(previous_file, encoding="utf-8") as stream:
    previous = json.load(stream)
with open(current_file, encoding="utf-8") as stream:
    current = json.load(stream)
print(f"前一清单: {previous_file}")
print(f"当前清单: {current_file}")
print(
    "前一结果: "
    + json.dumps(
        {
            "status": previous.get("status"),
            "exit_code": previous.get("exit_code"),
            "elapsed_seconds": previous.get("elapsed_seconds"),
            "metrics": previous.get("metrics", {}),
        },
        ensure_ascii=False,
    )
)
print(
    "当前结果: "
    + json.dumps(
        {
            "status": current.get("status"),
            "exit_code": current.get("exit_code"),
            "elapsed_seconds": current.get("elapsed_seconds"),
            "metrics": current.get("metrics", {}),
        },
        ensure_ascii=False,
    )
)
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
  micro [--debug] [--run-id ID|--resume ID]
                     运行 Google Benchmark 微基准
  qps [smoke|full] [--debug] [--run-id ID|--resume ID]
                     校验并运行全部 benchmark/qps_*.cpp 对应目标
  rps [profile]       压测已部署同源入口，profile=smoke|full|overload
  load [profile]      rps 的兼容别名
  diff micro <bench_target>
  diff qps <qps_target> <profile>
  diff rps <profile>  对比新报告结构中最近两个完整 manifest
  gen-data            生成 benchmark 数据文件
  build [--debug]     编译 benchmark 二进制
  check               仅执行本地依赖与目标发现检查
  -h, --help          显示帮助

主要环境变量:
  QPS_PROFILE=smoke|full
  QPS_TIMEOUT_SECONDS=<秒>
  BENCH_FLAGS="--benchmark_min_time=0.1s"
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
