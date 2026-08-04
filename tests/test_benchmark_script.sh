#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCHMARK_SOURCE="$PROJECT_ROOT/scripts/benchmark.sh"
TEST_TMP_DIR="$(mktemp -d)"
CURRENT_FIXTURE=""
BENCHMARK_LOG=""
BENCHMARK_RC=0
RUN_COUNTER=0
BENCHMARK_ENV=()

cleanup() {
  rm -rf "$TEST_TMP_DIR"
}
trap cleanup EXIT

fail() {
  printf '失败: %s\n' "$*" >&2
  return 1
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local description="$3"
  if [ "$expected" != "$actual" ]; then
    fail "$description，期望: [$expected]，实际: [$actual]"
  fi
}

assert_file() {
  local path="$1"
  local description="$2"
  [ -s "$path" ] || fail "$description: $path"
}

assert_contains() {
  local text="$1"
  local expected="$2"
  local description="$3"
  case "$text" in
    *"$expected"*) ;;
    *) fail "$description，未找到: [$expected]"; return 1 ;;
  esac
}

assert_not_contains() {
  local text="$1"
  local unexpected="$2"
  local description="$3"
  case "$text" in
    *"$unexpected"*) fail "$description，发现: [$unexpected]"; return 1 ;;
    *) ;;
  esac
}

snapshot_tree() {
  local directory="$1"
  local file
  [ -d "$directory" ] || return 0
  while IFS= read -r -d '' file; do
    /usr/bin/sha256sum "$file"
  done < <(/usr/bin/find "$directory" -type f -print0) | /usr/bin/sort
}

snapshot_path() {
  local path="$1"

  if [ -d "$path" ] && [ ! -L "$path" ]; then
    snapshot_tree "$path"
  elif [ -e "$path" ] || [ -L "$path" ]; then
    /usr/bin/sha256sum -- "$path"
  fi
}

write_fake_tools() {
  local root="$1"
  local fake_bin="$root/fake-bin"

  cat > "$fake_bin/xmake" <<'EOF'
#!/usr/bin/env bash
printf 'xmake <%s>\n' "$*" >> "$FAKE_TOOL_LOG"
exit 0
EOF

  cat > "$fake_bin/python3" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [ -n "${FAKE_PYTHON_LOG:-}" ]; then
  printf 'python3' >> "$FAKE_PYTHON_LOG"
  for argument in "$@"; do
    printf ' <%s>' "$argument" >> "$FAKE_PYTHON_LOG"
  done
  printf '\n' >> "$FAKE_PYTHON_LOG"
fi
exec /usr/bin/python3 "$@"
EOF

  cat > "$fake_bin/timeout" <<'EOF'
#!/usr/bin/env bash
printf 'timeout' >> "$FAKE_TOOL_LOG"
for argument in "$@"; do
  printf ' <%s>' "$argument" >> "$FAKE_TOOL_LOG"
done
printf '\n' >> "$FAKE_TOOL_LOG"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --signal=*|--kill-after=*|--foreground|--preserve-status)
      shift
      ;;
    --signal|--kill-after)
      [ "$#" -ge 2 ] || exit 64
      shift 2
      ;;
    [0-9]*s|[0-9]*m|[0-9]*h|[0-9]*)
      shift
      ;;
    *)
      break
      ;;
  esac
done
[ "$#" -gt 0 ] || exit 64
exec "$@"
EOF

  cat > "$fake_bin/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

output_file=""
write_status=0
url=""
curl_arguments=("$@")
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o|-w|-X|-H|--header|--data|--data-binary)
      option="$1"
      shift
      [ "$#" -gt 0 ] || exit 64
      value="$1"
      shift
      if [ "$option" = "-o" ]; then
        output_file="$value"
      elif [ "$option" = "-w" ]; then
        write_status=1
      fi
      ;;
    --connect-timeout|--max-time)
      shift
      [ "$#" -gt 0 ] || exit 64
      shift
      ;;
    -sS)
      shift
      ;;
    *)
      url="$1"
      shift
      ;;
  esac
done

printf 'curl' >> "$FAKE_TOOL_LOG"
for argument in "${curl_arguments[@]}"; do
  printf ' <%s>' "$argument" >> "$FAKE_TOOL_LOG"
done
printf ' <%s>\n' "$url" >> "$FAKE_TOOL_LOG"
status=200
payload='{}'
case "$url" in
  */api/health)
    if [ "${FAKE_HEALTH_FAIL:-0}" = 1 ]; then
      status=503
    else
      status=200
    fi
    payload='{}'
    ;;
  */api/auth/register)
    status=201
    payload='{"token":"ZmFrZS10b2tlbg==.0123456789abcdef"}'
    ;;
  */api/auth/login)
    status=200
    payload='{"token":"ZmFrZS10b2tlbg==.0123456789abcdef"}'
    ;;
  */api/files/upload)
    status=201
    payload='{"file_hash":"0000000000000000000000000000000000000000000000000000000000000000","file_id":1}'
    ;;
  *)
    status=200
    payload='{}'
    ;;
esac

if [ -n "$output_file" ]; then
  printf '%s\n' "$payload" > "$output_file"
fi
if [ "$write_status" -eq 1 ]; then
  printf '%s' "$status"
fi
EOF

  cat > "$fake_bin/wrk" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

lua_file=""
url=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -s)
      shift
      [ "$#" -gt 0 ] || exit 64
      lua_file="$1"
      shift
      ;;
    http://*|https://*)
      url="$1"
      shift
      ;;
    *)
      shift
      ;;
  esac
done
cell_id="$(basename "${lua_file:-none}" .lua)"
printf '%s\n' "$cell_id" >> "$FAKE_WRK_LOG"
[ -z "${FAKE_WRK_URL_LOG:-}" ] || printf '%s\n' "$url" >> "$FAKE_WRK_URL_LOG"
if [ -n "${FAKE_WRK_SLEEP_SECONDS:-}" ]; then
  sleep "$FAKE_WRK_SLEEP_SECONDS"
fi
failure_marker=""
if [ -n "${FAKE_WRK_FAIL_CELL:-}" ] && [ "$cell_id" = "$FAKE_WRK_FAIL_CELL" ]; then
  failure_marker="$FAKE_STATE_DIR/wrk-${cell_id}.failed-once"
elif [ "${FAKE_WRK_FAIL_ONCE:-0}" = "1" ]; then
  failure_marker="$FAKE_STATE_DIR/wrk.failed-once"
fi
if [ -n "$failure_marker" ] && [ ! -e "$failure_marker" ]; then
  : > "$failure_marker"
  printf 'synthetic wrk failure\n'
  exit 43
fi
if [ -n "${FAKE_WRK_TIMEOUT_CELL:-}" ] && [ "$cell_id" = "$FAKE_WRK_TIMEOUT_CELL" ]; then
  printf 'synthetic wrk timeout\n'
  exit 124
fi
if [ -n "${FAKE_WRK_OVERLOAD_CELL:-}" ] && [ "$cell_id" = "$FAKE_WRK_OVERLOAD_CELL" ]; then
  status_errors=1
  status_2xx=0
  status_non_2xx=1
else
  status_errors=0
  status_2xx=1
  status_non_2xx=0
fi
rps="${FAKE_WRK_RPS:-1000.0}"
for rps_override in ${FAKE_WRK_RPS_BY_CELL:-}; do
  case "$rps_override" in
    "$cell_id="*) rps="${rps_override#*=}"; break ;;
  esac
done
cat <<METRICS
Requests/sec: $rps
Latency 1ms
 50% 1ms
 90% 2ms
 99% 3ms
HPS_WRK_REQUESTS 1
HPS_WRK_STATUS_ERRORS $status_errors
HPS_SOCKET_CONNECT 0
HPS_SOCKET_READ 0
HPS_SOCKET_WRITE 0
HPS_SOCKET_TIMEOUT 0
HPS_STATUS_TOTAL 1
HPS_STATUS_2XX $status_2xx
HPS_STATUS_NON_2XX $status_non_2xx
HPS_STATUS_UNEXPECTED 0
HPS_STATUS_CODES 200=1
METRICS
EOF

  cat > "$fake_bin/dd" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
output_file=""
for argument in "$@"; do
  case "$argument" in
    of=*) output_file="${argument#of=}" ;;
  esac
done
[ -n "$output_file" ] || exit 64
printf 'fake-payload\n' > "$output_file"
EOF

  cat > "$fake_bin/find" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
for argument in "$@"; do
  case "$argument" in
    "$FAKE_LEGACY_DIR"|"$FAKE_LEGACY_DIR"/*)
      printf 'legacy-report-access <%s>\n' "$argument" >> "$FAKE_LEGACY_ACCESS_LOG"
      break
      ;;
  esac
done
exec /usr/bin/find "$@"
EOF

  cat > "$fake_bin/date" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -eq 1 ] && [ "$1" = '+%Y%m%d_%H%M%S' ]; then
  value=0
  if [ -f "$FAKE_DATE_STATE" ]; then
    value="$(<"$FAKE_DATE_STATE")"
  fi
  value=$((value + 1))
  printf '%s\n' "$value" > "$FAKE_DATE_STATE"
  printf '20260802_00%04d\n' "$value"
  exit 0
fi
exec /usr/bin/date "$@"
EOF

  cat > "$fake_bin/git" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

case "${1:-}" in
  rev-parse)
    printf 'testhash\n'
    ;;
  diff)
    if [ "${FAKE_GIT_TRACKED_DIRTY:-0}" = 1 ]; then
      exit 1
    fi
    exit 0
    ;;
  ls-files)
    if [ -n "${FAKE_GIT_UNTRACKED_PATHS:-}" ]; then
      null_delimited=0
      for argument in "$@"; do
        [ "$argument" = -z ] && null_delimited=1
      done
      while IFS= read -r path || [ -n "$path" ]; do
        if [ "$null_delimited" -eq 1 ]; then
          printf '%s\0' "$path"
        else
          printf '%s\n' "$path"
        fi
      done <<< "$FAKE_GIT_UNTRACKED_PATHS"
    fi
    ;;
esac
EOF

  cat > "$fake_bin/python3" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# diff 的当前实现以两个 JSON 输入调用 python3 -；禁止将旧目录作为这两个输入。
if [ "$#" -eq 3 ] && [ "$1" = "-" ]; then
  for argument in "$2" "$3"; do
    case "$argument" in
      "$FAKE_LEGACY_DIR"|"$FAKE_LEGACY_DIR"/*)
        printf 'legacy-report-python-input <%s>\n' "$argument" >> "$FAKE_LEGACY_ACCESS_LOG"
        break
        ;;
    esac
  done
fi
exec /usr/bin/python3 "$@"
EOF

  cat > "$fake_bin/hostname" <<'EOF'
#!/usr/bin/env bash
printf 'test-host\n'
EOF

  cat > "$fake_bin/nproc" <<'EOF'
#!/usr/bin/env bash
printf '2\n'
EOF

  cat > "$fake_bin/docker" <<'EOF'
#!/usr/bin/env bash
printf 'docker <%s>\n' "$*" >> "$FAKE_DOCKER_LOG"
exit 93
EOF

  chmod +x "$fake_bin"/*
}

write_target() {
  local root="$1"
  local target="$2"
  cat > "$root/bin/$target" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
target="$(basename "$0")"
printf '%s\n' "$target" >> "$FAKE_TARGET_LOG"
printf '%s' "$target" >> "$FAKE_TARGET_ARGS_LOG"
for argument in "$@"; do
  printf ' <%s>' "$argument" >> "$FAKE_TARGET_ARGS_LOG"
done
printf '\n' >> "$FAKE_TARGET_ARGS_LOG"
if [ -n "${FAKE_SWAP_RUN_DIR:-}" ]; then
  swap_marker="$FAKE_STATE_DIR/$target.run-dir-swapped"
  if [ ! -e "$swap_marker" ]; then
    mv -- "$FAKE_SWAP_RUN_DIR" "$FAKE_SWAP_MOVED_DIR"
    ln -s -- "$FAKE_SWAP_LINK_TARGET" "$FAKE_SWAP_RUN_DIR"
    : > "$swap_marker"
  fi
fi
case " ${FAKE_FAIL_ONCE_TARGETS:-} " in
  *" $target "*)
    marker="$FAKE_STATE_DIR/$target.failed-once"
    if [ ! -e "$marker" ]; then
      : > "$marker"
      printf 'synthetic target failure: %s\n' "$target"
      exit 41
    fi
    ;;
esac
case " ${FAKE_TIMEOUT_ONCE_TARGETS:-} " in
  *" $target "*)
    marker="$FAKE_STATE_DIR/$target.timeout-once"
    if [ ! -e "$marker" ]; then
      : > "$marker"
      printf 'synthetic target timeout: %s\n' "$target"
      exit 124
    fi
    ;;
esac
printf 'synthetic target output: %s\n' "$target"
EOF
  chmod +x "$root/bin/$target"
}

write_qps_source() {
  local root="$1"
  local target="$2"
  : > "$root/benchmark/$target.cpp"
}

write_legacy_report() {
  local root="$1"
  local kind="$2"
  local timestamp="$3"
  local marker="$4"
  local results='[]'
  case "$kind" in
    micro)
      results='[{"target":"bench_alpha","status":"passed"},{"target":"bench_beta","status":"passed"}]'
      ;;
    qps)
      results='[{"target":"qps_alpha","profile":"full","status":"passed"}]'
      ;;
    rps)
      results='[{"cell_id":"005_upload_1kb_c1_r1","status":"passed"}]'
      ;;
  esac
  cat > "$root/benchmark/reports/${kind}_${timestamp}.json" <<EOF
{
  "type": "${kind}",
  "summary": {"fixture_marker": "${marker}"},
  "results": ${results}
}
EOF
}

write_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local run_id="$5"
  local state="$6"
  local marker="$7"
  local directory="$root/benchmark/report/$kind/$selector/$run_id"
  local finished_at='null'
  local exit_code='null'
  local elapsed_seconds='null'

  if [ "$state" = passed ]; then
    finished_at='"2026-08-02T00:00:01Z"'
    exit_code=0
    elapsed_seconds=1
  fi

  mkdir -p "$directory"
  printf 'raw fixture: %s\n' "$marker" > "$directory/raw.txt"
  printf 'report fixture: %s\n' "$marker" > "$directory/report.txt"
  cat > "$directory/manifest.json" <<EOF
{
  "schema_version": 1,
  "kind": "${kind}",
  "target": "${selector}",
    "profile": "${profile}",
    "run_id": "${run_id}",
    "state": "${state}",
    "attempt": 1,
    "status": "${state}",
    "started_at": "2026-08-02T00:00:00Z",
    "finished_at": ${finished_at},
    "run_fingerprint": "fixture-${kind}-${selector}-${profile}-${run_id}",
    "fingerprint_inputs": {"fixture": "${marker}"},
    "environment": {"fixture": true},
    "exit_code": ${exit_code},
    "elapsed_seconds": ${elapsed_seconds},
    "raw_file": "raw.txt",
    "report_file": "report.txt",
    "result": {
      "status": "${state}",
      "exit_code": ${exit_code},
      "elapsed_seconds": ${elapsed_seconds}
    },
    "metrics": {"fixture_marker": "${marker}"}
}
EOF
}

write_misplaced_manifest() {
  local root="$1"
  local kind="$2"
  local manifest_target="$3"
  local manifest_profile="$4"
  local manifest_run_id="$5"
  local physical_selector="$6"
  local physical_run_id="$7"
  local marker="$8"
  local source="$root/benchmark/report/$kind/$manifest_target/$manifest_run_id"
  local destination="$root/benchmark/report/$kind/$physical_selector/$physical_run_id"

  write_manifest "$root" "$kind" "$manifest_target" "$manifest_profile" "$manifest_run_id" passed "$marker"
  mkdir -p "$(dirname "$destination")"
  mv "$source" "$destination"
}

set_manifest_string_field() {
  local manifest_file="$1"
  local field="$2"
  local value="$3"

  /usr/bin/python3 - "$manifest_file" "$field" "$value" <<'PY'
import json
import sys

manifest_file, field, value = sys.argv[1:]
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)
manifest[field] = value
with open(manifest_file, "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
}

write_incomplete_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local run_id="$5"
  local marker="$6"
  local manifest_file="$root/benchmark/report/$kind/$selector/$run_id/manifest.json"

  write_manifest "$root" "$kind" "$selector" "$profile" "$run_id" passed "$marker"
  /usr/bin/python3 - "$manifest_file" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    manifest = json.load(stream)
del manifest["attempt"]
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
}

write_missing_artifact_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local run_id="$5"
  local marker="$6"
  local directory="$root/benchmark/report/$kind/$selector/$run_id"

  write_manifest "$root" "$kind" "$selector" "$profile" "$run_id" passed "$marker"
  rm -f "$directory/report.txt"
}

set_manifest_boolean_integer() {
  local manifest_file="$1"
  local field="$2"

  /usr/bin/python3 - "$manifest_file" "$field" <<'PY'
import json
import sys

manifest_file, field = sys.argv[1:]
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)
if field == "schema_version":
    manifest[field] = True
elif field in {"attempt", "exit_code"}:
    manifest[field] = False
elif field == "result.exit_code":
    manifest["result"]["exit_code"] = False
else:
    raise SystemExit(f"unsupported boolean integer field: {field}")
with open(manifest_file, "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
}

write_boolean_integer_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local run_id="$5"
  local marker="$6"
  local field="$7"
  local manifest_file="$root/benchmark/report/$kind/$selector/$run_id/manifest.json"

  write_manifest "$root" "$kind" "$selector" "$profile" "$run_id" passed "$marker"
  set_manifest_boolean_integer "$manifest_file" "$field"
}

set_manifest_artifact_paths() {
  local manifest_file="$1"
  local raw_file="$2"
  local report_file="$3"

  /usr/bin/python3 - "$manifest_file" "$raw_file" "$report_file" <<'PY'
import json
import sys

manifest_file, raw_file, report_file = sys.argv[1:]
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)
manifest["raw_file"] = raw_file
manifest["report_file"] = report_file
with open(manifest_file, "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
}

write_artifact_path_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local run_id="$5"
  local marker="$6"
  local raw_file="$7"
  local report_file="$8"
  local directory="$root/benchmark/report/$kind/$selector/$run_id"
  local artifact

  write_manifest "$root" "$kind" "$selector" "$profile" "$run_id" passed "$marker"
  for artifact in "$raw_file" "$report_file"; do
    case "$artifact" in
      */*)
        mkdir -p "$(dirname "$directory/$artifact")"
        printf 'nested artifact fixture: %s\n' "$marker" > "$directory/$artifact"
        ;;
    esac
  done
  set_manifest_artifact_paths "$directory/manifest.json" "$raw_file" "$report_file"
}

write_symlink_artifact_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local run_id="$5"
  local marker="$6"
  local artifact="$7"
  local directory="$root/benchmark/report/$kind/$selector/$run_id"
  local outside="$root/artifact-outside/${kind}-${selector}-${run_id}-${artifact}"

  write_manifest "$root" "$kind" "$selector" "$profile" "$run_id" passed "$marker"
  mkdir -p "$(dirname "$outside")"
  printf 'outside artifact fixture: %s\n' "$marker" > "$outside"
  rm -f "$directory/$artifact"
  ln -s "$outside" "$directory/$artifact"
}

write_non_object_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local run_id="$4"
  local value="$5"
  local directory="$root/benchmark/report/$kind/$selector/$run_id"

  mkdir -p "$directory"
  printf '%s\n' "$value" > "$directory/manifest.json"
}

write_non_utf8_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local run_id="$4"
  local directory="$root/benchmark/report/$kind/$selector/$run_id"

  mkdir -p "$directory"
  printf '\377\376\n' > "$directory/manifest.json"
}

write_legacy_aggregate_manifest() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local marker="$5"
  local run_id=20990101_000000
  local original="$root/benchmark/report/$kind/$selector/$run_id"
  local aggregate="$root/benchmark/report/$kind/_legacy_aggregate/$selector/$run_id"

  write_manifest "$root" "$kind" "$selector" "$profile" "$run_id" passed "$marker"
  mkdir -p "$(dirname "$aggregate")"
  mv "$original" "$aggregate"
}

write_diff_manifest_series() {
  local root="$1"
  local kind="$2"
  local selector="$3"
  local profile="$4"
  local marker_prefix="$5"

  write_manifest "$root" "$kind" "$selector" "$profile" 20260802_000001 passed "${marker_prefix}_oldest_complete"
  write_manifest "$root" "$kind" "$selector" "$profile" 20260802_000002 passed "${marker_prefix}_previous_complete"
  write_manifest "$root" "$kind" "$selector" "$profile" 20260802_000003 passed "${marker_prefix}_latest_complete"
  write_manifest "$root" "$kind" "$selector" "$profile" 20260802_000004 running "${marker_prefix}_latest_running"
  write_incomplete_manifest "$root" "$kind" "$selector" "$profile" 20260802_000005 "${marker_prefix}_missing_attempt"
  write_missing_artifact_manifest "$root" "$kind" "$selector" "$profile" 20260802_000006 "${marker_prefix}_missing_report"
  write_boolean_integer_manifest "$root" "$kind" "$selector" "$profile" 20260802_000007 \
    "${marker_prefix}_boolean_schema" schema_version
  write_boolean_integer_manifest "$root" "$kind" "$selector" "$profile" 20260802_000008 \
    "${marker_prefix}_boolean_attempt" attempt
  write_boolean_integer_manifest "$root" "$kind" "$selector" "$profile" 20260802_000009 \
    "${marker_prefix}_boolean_exit_code" exit_code
  write_boolean_integer_manifest "$root" "$kind" "$selector" "$profile" 20260802_000010 \
    "${marker_prefix}_boolean_result_exit_code" result.exit_code
  write_artifact_path_manifest "$root" "$kind" "$selector" "$profile" 20260802_000011 \
    "${marker_prefix}_manifest_as_raw" manifest.json report.txt
  write_artifact_path_manifest "$root" "$kind" "$selector" "$profile" 20260802_000012 \
    "${marker_prefix}_shared_artifact" raw.txt raw.txt
  write_artifact_path_manifest "$root" "$kind" "$selector" "$profile" 20260802_000013 \
    "${marker_prefix}_nested_raw" nested/raw.txt report.txt
  write_symlink_artifact_manifest "$root" "$kind" "$selector" "$profile" 20260802_000014 \
    "${marker_prefix}_raw_symlink" raw.txt
  write_symlink_artifact_manifest "$root" "$kind" "$selector" "$profile" 20260802_000015 \
    "${marker_prefix}_report_symlink" report.txt
  write_non_object_manifest "$root" "$kind" "$selector" 20260802_000016 '[]'
  write_non_object_manifest "$root" "$kind" "$selector" 20260802_000017 '"not-an-object"'
  write_non_object_manifest "$root" "$kind" "$selector" 20260802_000018 'null'
  write_non_utf8_manifest "$root" "$kind" "$selector" 20260802_000024
  write_legacy_aggregate_manifest "$root" "$kind" "$selector" "$profile" "${marker_prefix}_AGGREGATE_POISON"
}

setup_fixture() {
  local name="$1"
  CURRENT_FIXTURE="$TEST_TMP_DIR/$name"
  mkdir -p "$CURRENT_FIXTURE/scripts/lib" "$CURRENT_FIXTURE/benchmark" \
    "$CURRENT_FIXTURE/bin" "$CURRENT_FIXTURE/fake-bin" "$CURRENT_FIXTURE/state"
  cp "$BENCHMARK_SOURCE" "$CURRENT_FIXTURE/scripts/benchmark.sh"
  cp "$PROJECT_ROOT/scripts/lib/isolated_docker_env.sh" "$CURRENT_FIXTURE/scripts/lib/isolated_docker_env.sh"
  chmod +x "$CURRENT_FIXTURE/scripts/benchmark.sh"
  cat > "$CURRENT_FIXTURE/scripts/lib/common.sh" <<EOF
#!/usr/bin/env bash
PROJECT_ROOT="$CURRENT_FIXTURE"
FRONTEND_DIR="\$PROJECT_ROOT/frontend"
red() { printf '%s\\n' "\$*"; }
green() { printf '%s\\n' "\$*"; }
yellow() { printf '%s\\n' "\$*"; }
blue() { printf '%s\\n' "\$*"; }
require_tools() {
  local missing=0
  local tool
  for tool in "\$@"; do
    command -v "\$tool" >/dev/null 2>&1 || missing=1
  done
  return "\$missing"
}
EOF
  chmod +x "$CURRENT_FIXTURE/scripts/lib/common.sh"
  cat > "$CURRENT_FIXTURE/scripts/docker.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$*" >> "$FAKE_DOCKER_LOG"
case "${1:-}" in
  deploy)
    [ "${FAKE_DOCKER_DEPLOY_FAIL:-0}" != 1 ] || exit 71
    ;;
  base-url)
    [ "${FAKE_DOCKER_BASE_URL_FAIL:-0}" != 1 ] || exit 72
    printf '%s\n' "${FAKE_DOCKER_BASE_URL:-http://managed.fake}"
    ;;
  runtime-fingerprint)
    [ "${FAKE_DOCKER_RUNTIME_FINGERPRINT_FAIL:-0}" != 1 ] || exit 73
    if [ -n "${FAKE_DOCKER_RUNTIME_FINGERPRINT_SLEEP_SECONDS:-}" ]; then
      sleep "$FAKE_DOCKER_RUNTIME_FINGERPRINT_SLEEP_SECONDS"
    fi
    if [ "${FAKE_DOCKER_RUNTIME_FINGERPRINT+x}" = x ]; then
      printf '%s\n' "$FAKE_DOCKER_RUNTIME_FINGERPRINT"
    else
      printf '%s\n' managed-runtime-v1
    fi
    ;;
  down)
    if [ -n "${FAKE_DOCKER_REPORT_ROOT:-}" ]; then
      for profile in ${FAKE_DOCKER_REPORT_PROFILES:-}; do
        report_dir="$FAKE_DOCKER_REPORT_ROOT/$profile/$FAKE_DOCKER_REPORT_RUN_ID"
        if ! { [ -s "$report_dir/report.json" ] && [ -s "$report_dir/report.txt" ] && [ -s "$report_dir/manifest.json" ]; }; then
          [ -z "${FAKE_DOCKER_ORDER_LOG:-}" ] || printf '%s\n' 'down-missing-reports' >> "$FAKE_DOCKER_ORDER_LOG"
          exit 74
        fi
      done
      [ -z "${FAKE_DOCKER_ORDER_LOG:-}" ] || printf '%s\n' 'down-reports-ready' >> "$FAKE_DOCKER_ORDER_LOG"
    fi
    [ "${FAKE_DOCKER_DOWN_FAIL:-0}" != 1 ] || exit 68
    ;;
esac
EOF
  chmod +x "$CURRENT_FIXTURE/scripts/docker.sh"

  : > "$CURRENT_FIXTURE/target.log"
  : > "$CURRENT_FIXTURE/target-args.log"
  : > "$CURRENT_FIXTURE/wrk.log"
  : > "$CURRENT_FIXTURE/wrk-url.log"
  : > "$CURRENT_FIXTURE/tools.log"
  : > "$CURRENT_FIXTURE/python.log"
  : > "$CURRENT_FIXTURE/docker.log"
  : > "$CURRENT_FIXTURE/legacy-access.log"
  : > "$CURRENT_FIXTURE/date.state"
  mkdir -p "$CURRENT_FIXTURE/benchmark/reports"
  write_legacy_report "$CURRENT_FIXTURE" micro 20990101_000000 LEGACY_MICRO_BASELINE
  write_legacy_report "$CURRENT_FIXTURE" qps 20990101_000000 LEGACY_QPS_BASELINE
  write_legacy_report "$CURRENT_FIXTURE" rps 20990101_000000 LEGACY_RPS_BASELINE
  write_fake_tools "$CURRENT_FIXTURE"
  BENCHMARK_ENV=()
}

run_benchmark() {
  local root="$1"
  shift
  RUN_COUNTER=$((RUN_COUNTER + 1))
  BENCHMARK_LOG="$root/run-${RUN_COUNTER}.log"
  if env \
    "PATH=$root/fake-bin:$PATH" \
    "FAKE_TARGET_LOG=$root/target.log" \
    "FAKE_TARGET_ARGS_LOG=$root/target-args.log" \
    "FAKE_WRK_LOG=$root/wrk.log" \
    "FAKE_WRK_URL_LOG=$root/wrk-url.log" \
    "FAKE_TOOL_LOG=$root/tools.log" \
    "FAKE_PYTHON_LOG=$root/python.log" \
    "FAKE_DOCKER_LOG=$root/docker.log" \
    "FAKE_LEGACY_DIR=$root/benchmark/reports" \
    "FAKE_LEGACY_ACCESS_LOG=$root/legacy-access.log" \
    "FAKE_DATE_STATE=$root/date.state" \
    "FAKE_STATE_DIR=$root/state" \
    "RPS_BASE_URL=http://fake.invalid" \
    "${BENCHMARK_ENV[@]}" \
    bash "$root/scripts/benchmark.sh" "$@" > "$BENCHMARK_LOG" 2>&1; then
    BENCHMARK_RC=0
  else
    BENCHMARK_RC=$?
  fi
  return 0
}

print_benchmark_log() {
  local file="$1"
  printf '%s\n' "--- 完整基准脚本输出: $file ---" >&2
  while IFS= read -r line || [ -n "$line" ]; do
    printf '%s\n' "$line" >&2
  done < "$file"
  printf '%s\n' '--- 基准脚本输出结束 ---' >&2
}

assert_benchmark_status() {
  local expected="$1"
  local description="$2"
  if [ "$BENCHMARK_RC" -ne "$expected" ]; then
    print_benchmark_log "$BENCHMARK_LOG"
    fail "$description，期望退出码 $expected，实际为 $BENCHMARK_RC"
  fi
}

manifest_paths() {
  local base="$1"
  [ -d "$base" ] || return 0
  /usr/bin/find "$base" -mindepth 2 -maxdepth 2 -type f -name manifest.json -print | /usr/bin/sort
}

discover_single_run_id() {
  local base="$1"
  local description="$2"
  local -a paths=()
  if [ -d "$base" ]; then
    mapfile -t paths < <(manifest_paths "$base")
  fi
  if [ "${#paths[@]}" -ne 1 ]; then
    fail "$description 应恰好生成一个目标级 manifest.json，实际为 ${#paths[@]} 个，预期路径: $base/<run-id>/manifest.json"
    return 1
  fi
  [ -s "${paths[0]}" ] || {
    fail "$description 生成的 manifest.json 为空: ${paths[0]}"
    return 1
  }
  basename "$(dirname "${paths[0]}")"
}

assert_target_lines() {
  local file="$1"
  local description="$2"
  shift 2
  local expected actual
  expected="$(printf '%s\n' "$@")"
  if [ -f "$file" ]; then
    actual="$(<"$file")"
  else
    actual=""
  fi
  assert_equals "$expected" "$actual" "$description"
}

assert_recorded_lines() {
  local file="$1"
  local description="$2"
  shift 2
  local expected actual
  expected="$(printf '%s\n' "$@")"
  if [ -f "$file" ]; then
    actual="$(<"$file")"
  else
    actual=""
  fi
  assert_equals "$expected" "$actual" "$description"
}

assert_empty_file() {
  local file="$1"
  local description="$2"
  if [ -s "$file" ]; then
    fail "$description，实际记录: [$(<"$file")]"
    return 1
  fi
}

assert_single_link_regular_file() {
  local file="$1"
  local description="$2"
  local link_count
  if [ ! -f "$file" ] || [ -L "$file" ]; then
    fail "$description 必须是当前目录中的普通文件: $file"
    return 1
  fi
  link_count="$(stat -c '%h' "$file")"
  if ! assert_equals 1 "$link_count" "$description 必须只有一个硬链接"; then
    return 1
  fi
}

assert_manifest_passed() {
  local manifest_file="$1"
  local description="$2"

  if ! /usr/bin/python3 - "$manifest_file" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    manifest = json.load(stream)
assert manifest["state"] == "passed"
assert manifest["status"] == "passed"
assert manifest["exit_code"] == 0
assert manifest["result"]["status"] == "passed"
assert manifest["result"]["exit_code"] == 0
PY
  then
    fail "$description 必须是 passed 清单: $manifest_file"
    return 1
  fi
}

assert_legacy_not_written() {
  local root="$1"
  local before="$2"
  local after
  after="$(snapshot_tree "$root/benchmark/reports")"
  if [ "$before" != "$after" ]; then
    fail '旧 benchmark/reports 不应被新逻辑写入（夹具树快照发生变化）'
    return 1
  fi
}

report_legacy_access() {
  local root="$1"
  if [ -s "$root/legacy-access.log" ]; then
    printf '诊断: 旧 benchmark/reports 被访问；是否失败由毒化数据、目标调用和 diff 输出断言决定\n' >&2
  fi
}

assert_no_docker() {
  local root="$1"
  [ ! -s "$root/docker.log" ] || fail '测试不得访问真实 Docker 服务'
}

assert_docker_operation_count() {
  local root="$1"
  local operation="$2"
  local expected="$3"
  local description="$4"
  local actual
  actual="$(awk -v operation="$operation" '$1 == operation { count += 1 } END { print count + 0 }' "$root/docker.log")"
  assert_equals "$expected" "$actual" "$description"
}

assert_rps_failed_profile_artifacts() {
  local root="$1"
  local profile="$2"
  local run_id="$3"
  local exit_code="$4"
  local description="$5"
  local run_dir="$root/benchmark/report/rps/$profile/$run_id"

  if ! /usr/bin/python3 - "$run_dir" "$profile" "$run_id" "$exit_code" <<'PY'
import json
import os
import sys

run_dir, profile, run_id, exit_code = sys.argv[1:]
exit_code = int(exit_code)
for name in ("report.json", "report.txt", "manifest.json"):
    path = os.path.join(run_dir, name)
    assert os.path.isfile(path) and not os.path.islink(path) and os.path.getsize(path) > 0, path
with open(os.path.join(run_dir, "report.json"), encoding="utf-8") as stream:
    report = json.load(stream)
assert report["summary"]["result"] == "failed"
assert report["summary"]["preflight_status"] == "failed"
with open(os.path.join(run_dir, "report.txt"), encoding="utf-8") as stream:
    assert "结论: failed" in stream.read()
with open(os.path.join(run_dir, "manifest.json"), encoding="utf-8") as stream:
    manifest = json.load(stream)
assert manifest["profile"] == profile and manifest["run_id"] == run_id
assert manifest["state"] == "failed" and manifest["status"] == "failed"
assert manifest["exit_code"] == exit_code
assert manifest["result"]["status"] == "failed"
assert manifest["result"]["exit_code"] == exit_code
cells = manifest["cells"]
assert isinstance(cells, dict)
assert all(
    isinstance(cell, dict)
    and cell.get("state") in {"passed", "failed", "timeout", "overloaded"}
    and cell.get("status") == cell.get("state")
    for cell in cells.values()
)
PY
  then
    fail "$description 必须保留非空 failed RPS report 和 manifest: $run_dir"
    return 1
  fi
}

assert_manifest_contract() {
  local manifest_file="$1"
  local expected_kind="$2"
  local expected_target="$3"
  local expected_profile="$4"
  local expected_run_id="$5"
  local description="$6"

  if ! /usr/bin/python3 - "$manifest_file" "$expected_kind" "$expected_target" \
    "$expected_profile" "$expected_run_id" <<'PY'
import json
import sys

manifest_file, expected_kind, expected_target, expected_profile, expected_run_id = sys.argv[1:]
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)

required_fields = {
    "schema_version", "kind", "target", "profile", "run_id", "state", "attempt",
    "started_at", "finished_at", "run_fingerprint", "fingerprint_inputs", "environment",
    "status", "exit_code", "elapsed_seconds", "raw_file", "report_file", "result", "metrics",
}
missing_fields = required_fields.difference(manifest)
assert not missing_fields, f"missing fields: {sorted(missing_fields)}"
assert manifest["schema_version"] == 1
assert manifest["kind"] == expected_kind
assert manifest["target"] == expected_target
assert manifest["profile"] == expected_profile
assert manifest["run_id"] == expected_run_id
assert manifest["state"] == "passed"
assert manifest["status"] == "passed"
assert manifest["attempt"] >= 1
assert manifest["finished_at"]
assert manifest["exit_code"] == 0
assert manifest["raw_file"] == "raw.txt"
assert manifest["report_file"] == "report.txt"
assert manifest["result"]["status"] == "passed"
assert manifest["result"]["exit_code"] == 0
for field in ("binary_sha256", "build_mode", "git_hash", "profile", "target", "timeout_seconds"):
    assert field in manifest["fingerprint_inputs"], f"missing fingerprint input: {field}"
if expected_kind == "micro":
    digest = manifest["fingerprint_inputs"].get("bench_flags_sha256")
    assert isinstance(digest, str) and len(digest) == 64

def contains_secret_key(value):
    if isinstance(value, dict):
        return any(
            key.lower() in {"token", "password"} or contains_secret_key(child)
            for key, child in value.items()
        )
    if isinstance(value, list):
        return any(contains_secret_key(child) for child in value)
    return False

assert not contains_secret_key(manifest), "manifest contains a token or password field"
PY
  then
    fail "$description 清单字段、指纹或敏感字段断言失败: $manifest_file"
    return 1
  fi
}

assert_manifest_execution_state() {
  local manifest_file="$1"
  local expected_state="$2"
  local expected_attempt="$3"
  local expected_exit_code="$4"
  local description="$5"

  if ! /usr/bin/python3 - "$manifest_file" "$expected_state" "$expected_attempt" "$expected_exit_code" <<'PY'
import json
import sys

manifest_file, expected_state, expected_attempt, expected_exit_code = sys.argv[1:]
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)

expected_attempt = int(expected_attempt)
expected_exit_code = None if expected_exit_code == "null" else int(expected_exit_code)
assert manifest["state"] == expected_state
assert manifest["status"] == expected_state
assert manifest["attempt"] == expected_attempt
assert manifest["exit_code"] == expected_exit_code
assert manifest["result"]["status"] == expected_state
assert manifest["result"]["exit_code"] == expected_exit_code
PY
  then
    fail "$description 清单状态、尝试次数或退出码断言失败: $manifest_file"
    return 1
  fi
}

assert_diff_selects_latest_complete_pair() {
  local output_file="$1"
  local description="$2"
  local legacy_marker="$3"
  local aggregate_marker="$4"
  local output failures=0
  output="$(<"$output_file")"
  if ! assert_contains "$output" 20260802_000002 "$description 应选择倒数第二份 passed manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_contains "$output" 20260802_000003 "$description 应选择最近一份 passed manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" 20260802_000001 "$description 不应选择更早的 passed manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" 20260802_000004 "$description 必须跳过最新但未完成的 manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" 20260802_000005 "$description 必须跳过缺少必需字段的 manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" 20260802_000006 "$description 必须跳过缺少 artifact 的 manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" 20260802_000007 "$description 必须跳过 schema_version=true 的 manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" 20260802_000008 "$description 必须跳过 attempt=false 的 manifest"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" 20260802_000009 "$description 必须跳过 exit_code=false 的 manifest"; then
    failures=$((failures + 1))
  fi
  local invalid_run_id
  for invalid_run_id in \
    20260802_000010 \
    20260802_000011 \
    20260802_000012 \
    20260802_000013 \
    20260802_000014 \
    20260802_000015 \
    20260802_000016 \
    20260802_000017 \
    20260802_000018 \
    20260802_000019 \
    20260802_000020 \
    20260802_000021 \
    20260802_000022 \
    20260802_000023 \
    20260802_000024; do
    if ! assert_not_contains "$output" "$invalid_run_id" "$description 必须跳过不完整或毒化的 manifest"; then
      failures=$((failures + 1))
    fi
  done
  if ! assert_not_contains "$output" Traceback "$description 遇到损坏或非 UTF-8 JSON 时不得输出 Python traceback"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" "$legacy_marker" "$description 不得输出旧格式遗留标记"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$output" "$aggregate_marker" "$description 不得扫描 _legacy_aggregate"; then
    failures=$((failures + 1))
  fi
  [ "$failures" -eq 0 ]
}

configure_rps_environment() {
  local read_concurrency="$1"
  local target_fingerprint="$2"
  BENCHMARK_ENV=(
    "RPS_READ_CONCURRENCY=$read_concurrency"
    RPS_TRANSFER_CONCURRENCY=1
    RPS_UPLOAD_CONCURRENCY=1
    RPS_REPEATS=1
    RPS_DURATION_SECONDS=1
    RPS_UPLOAD_DURATION_SECONDS=1
    RPS_UPLOAD_DELAY_MILLISECONDS=0
    RPS_CELL_TIMEOUT_GRACE_SECONDS=1
    RPS_REQUEST_TIMEOUT_SECONDS=1
    "RPS_TARGET_FINGERPRINT=$target_fingerprint"
  )
}

test_run_id_and_manifest_contract() {
  local failures=0
  setup_fixture run-id-contract
  local root="$CURRENT_FIXTURE"
  local micro_run_id=20260802_120000
  local qps_run_id=20260802_120001
  local micro_manifest="$root/benchmark/report/micro/bench_alpha/$micro_run_id/manifest.json"
  local qps_manifest="$root/benchmark/report/qps/qps_alpha/$qps_run_id/manifest.json"
  write_target "$root" bench_alpha
  write_target "$root" qps_alpha
  write_qps_source "$root" qps_alpha
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"

  run_benchmark "$root" micro --run-id "$micro_run_id"
  if ! assert_benchmark_status 0 '显式微基准运行 ID'; then failures=$((failures + 1)); fi
  if ! assert_file "$root/benchmark/report/micro/bench_alpha/$micro_run_id/raw.txt" '微基准原始输出'; then
    failures=$((failures + 1))
  fi
  if ! assert_file "$root/benchmark/report/micro/bench_alpha/$micro_run_id/report.txt" '微基准文本报告'; then
    failures=$((failures + 1))
  fi
  if ! assert_manifest_contract "$micro_manifest" micro bench_alpha default "$micro_run_id" '微基准'; then
    failures=$((failures + 1))
  fi

  : > "$root/target.log"
  run_benchmark "$root" micro --resume "$micro_run_id"
  if ! assert_benchmark_status 0 '相同指纹微基准续跑跳过'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/target.log" '同指纹通过微基准不应重跑'; then failures=$((failures + 1)); fi

  if ! /usr/bin/python3 - "$micro_manifest" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    manifest = json.load(stream)
manifest["state"] = "running"
manifest["status"] = "running"
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
  then
    fail '无法构造 running 微基准清单夹具'
    failures=$((failures + 1))
  fi
  : > "$root/target.log"
  run_benchmark "$root" micro --resume "$micro_run_id"
  if ! assert_benchmark_status 0 'running 微基准清单续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" 'running 清单必须重跑目标' bench_alpha; then
    failures=$((failures + 1))
  fi

  printf '{damaged manifest\n' > "$micro_manifest"
  : > "$root/target.log"
  run_benchmark "$root" micro --resume "$micro_run_id"
  if ! assert_benchmark_status 0 '损坏微基准清单续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" '损坏清单必须重跑目标' bench_alpha; then
    failures=$((failures + 1))
  fi

  : > "$root/target.log"
  run_benchmark "$root" micro --run-id "$micro_run_id"
  if ! assert_benchmark_status 1 '新微基准运行不得覆盖目标目录'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/target.log" '覆盖被拒绝时不得执行微基准目标'; then failures=$((failures + 1)); fi

  run_benchmark "$root" micro --run-id 20260802-120000
  if ! assert_benchmark_status 2 '非法微基准运行 ID'; then failures=$((failures + 1)); fi
  run_benchmark "$root" micro --run-id 20260802_120002 --resume 20260802_120002
  if ! assert_benchmark_status 2 '互斥的微基准运行选项'; then failures=$((failures + 1)); fi

  run_benchmark "$root" qps smoke --run-id "$qps_run_id"
  if ! assert_benchmark_status 0 '显式 QPS 运行 ID'; then failures=$((failures + 1)); fi
  if ! assert_file "$root/benchmark/report/qps/qps_alpha/$qps_run_id/raw.txt" 'QPS 原始输出'; then
    failures=$((failures + 1))
  fi
  if ! assert_file "$root/benchmark/report/qps/qps_alpha/$qps_run_id/report.txt" 'QPS 文本报告'; then
    failures=$((failures + 1))
  fi
  if ! assert_manifest_contract "$qps_manifest" qps qps_alpha smoke "$qps_run_id" 'QPS'; then
    failures=$((failures + 1))
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_generated_reports_preserve_git_fingerprint() {
  local failures=0
  setup_fixture generated-report-git-state
  local root="$CURRENT_FIXTURE"
  local run_id=20260802_121000
  local report_path="benchmark/report/micro/bench_alpha/$run_id/manifest.json"
  write_target "$root" bench_alpha
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"

  run_benchmark "$root" micro --run-id "$run_id"
  if ! assert_benchmark_status 0 '报告 Git 指纹首次微基准运行'; then failures=$((failures + 1)); fi

  : > "$root/target.log"
  BENCHMARK_ENV=("FAKE_GIT_UNTRACKED_PATHS=$report_path")
  run_benchmark "$root" micro --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '仅报告未跟踪路径的微基准续跑'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/target.log" '仅报告未跟踪路径不得使已通过目标重跑'; then
    failures=$((failures + 1))
  fi
  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi

  setup_fixture untracked-business-git-state
  root="$CURRENT_FIXTURE"
  run_id=20260802_121001
  write_target "$root" bench_alpha
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"
  run_benchmark "$root" micro --run-id "$run_id"
  if ! assert_benchmark_status 0 '业务未跟踪路径首次微基准运行'; then failures=$((failures + 1)); fi
  : > "$root/target.log"
  BENCHMARK_ENV=(FAKE_GIT_UNTRACKED_PATHS=src/business_change.cpp)
  run_benchmark "$root" micro --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '业务未跟踪路径的微基准续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" '业务未跟踪路径必须使目标重跑' bench_alpha; then
    failures=$((failures + 1))
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi

  setup_fixture tracked-source-git-state
  root="$CURRENT_FIXTURE"
  run_id=20260802_121002
  write_target "$root" bench_alpha
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"
  run_benchmark "$root" micro --run-id "$run_id"
  if ! assert_benchmark_status 0 '已修改业务源首次微基准运行'; then failures=$((failures + 1)); fi
  : > "$root/target.log"
  BENCHMARK_ENV=(FAKE_GIT_TRACKED_DIRTY=1)
  run_benchmark "$root" micro --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '已修改业务源的微基准续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" '已修改业务源必须使目标重跑' bench_alpha; then
    failures=$((failures + 1))
  fi
  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_bench_flags_are_hashed_and_sensitive_values_are_rejected() {
  local failures=0
  setup_fixture normal-bench-flags
  local root="$CURRENT_FIXTURE"
  local run_id=20260802_121100
  local manifest_file="$root/benchmark/report/micro/bench_alpha/$run_id/manifest.json"
  local normal_flags='--benchmark_min_time=0.1s --benchmark_filter=Alpha'
  write_target "$root" bench_alpha
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"

  BENCHMARK_ENV=("BENCH_FLAGS=$normal_flags")
  run_benchmark "$root" micro --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '正常 BENCH_FLAGS 首次微基准运行'; then failures=$((failures + 1)); fi
  local manifest_text=""
  if [ -f "$manifest_file" ]; then
    manifest_text="$(<"$manifest_file")"
  fi
  if ! assert_contains "$manifest_text" bench_flags_sha256 '清单应保存 BENCH_FLAGS 摘要'; then
    failures=$((failures + 1))
  fi

  : > "$root/target.log"
  BENCHMARK_ENV=("BENCH_FLAGS=$normal_flags")
  run_benchmark "$root" micro --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '相同正常 BENCH_FLAGS 的续跑'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/target.log" '相同正常 BENCH_FLAGS 不应重跑目标'; then
    failures=$((failures + 1))
  fi

  : > "$root/target.log"
  BENCHMARK_ENV=("BENCH_FLAGS=--benchmark_min_time=0.2s --benchmark_filter=Alpha")
  run_benchmark "$root" micro --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '变化正常 BENCH_FLAGS 的续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" '变化正常 BENCH_FLAGS 必须重跑目标' bench_alpha; then
    failures=$((failures + 1))
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi

  local -a sensitive_flags=(
    '--api-token=BENCH_FLAG_TOKEN'
    '--password=BENCH_FLAG_PASSWORD'
    '--client-secret=BENCH_FLAG_SECRET'
    '--credential=BENCH_FLAG_CREDENTIAL'
    '--api-key=BENCH_FLAG_API_KEY'
  )
  local index sensitive_flags_value secret_value output case_label sensitive_run_id
  for index in "${!sensitive_flags[@]}"; do
    sensitive_flags_value="${sensitive_flags[$index]}"
    secret_value="${sensitive_flags_value#*=}"
    case_label="敏感参数第 $((index + 1)) 项"
    printf -v sensitive_run_id '20260802_1211%02d' "$index"
    setup_fixture "sensitive-bench-flags-$index"
    root="$CURRENT_FIXTURE"
    write_target "$root" bench_alpha
    BENCHMARK_ENV=("BENCH_FLAGS=--benchmark_min_time=0.1s $sensitive_flags_value")
    run_benchmark "$root" micro --run-id "$sensitive_run_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 2 "$case_label 必须在执行前拒绝"; then
      failures=$((failures + 1))
    fi
    if [ -e "$root/benchmark/report" ]; then
      fail "$case_label 被拒绝时不得创建报告或原始输出"
      failures=$((failures + 1))
    fi
    if ! assert_empty_file "$root/target.log" "$case_label 被拒绝时不得执行目标"; then
      failures=$((failures + 1))
    fi
    if [ -s "$root/target-args.log" ]; then
      fail "$case_label 被拒绝时不得传给目标"
      failures=$((failures + 1))
    fi
    if ! assert_empty_file "$root/tools.log" "$case_label 被拒绝时不得执行 xmake"; then
      failures=$((failures + 1))
    fi
    output="$(<"$BENCHMARK_LOG")"
    case "$output" in
      *"$secret_value"*) fail "$case_label 不得回显秘密值"; failures=$((failures + 1)) ;;
    esac
  done
  [ "$failures" -eq 0 ]
}

test_boolean_manifest_integers_are_rejected() {
  local failures=0
  setup_fixture boolean-manifest-integers
  local root="$CURRENT_FIXTURE"
  local target=bench_alpha
  local -a fields=(schema_version attempt exit_code result.exit_code)
  local -a run_ids=(20260802_121301 20260802_121302 20260802_121303 20260802_121304)
  write_target "$root" "$target"
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"
  local index field run_id manifest_file

  for index in "${!fields[@]}"; do
    field="${fields[$index]}"
    run_id="${run_ids[$index]}"
    manifest_file="$root/benchmark/report/micro/$target/$run_id/manifest.json"
    run_benchmark "$root" micro --run-id "$run_id"
    if ! assert_benchmark_status 0 "布尔 $field 首次微基准运行"; then failures=$((failures + 1)); fi
    if ! set_manifest_boolean_integer "$manifest_file" "$field"; then
      fail "无法构造 $field 布尔清单夹具"
      failures=$((failures + 1))
    fi
    : > "$root/target.log"
    run_benchmark "$root" micro --resume "$run_id"
    if ! assert_benchmark_status 0 "$field 布尔清单续跑"; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$root/target.log" "$field=false/true 清单必须重跑" "$target"; then
      failures=$((failures + 1))
    fi
  done

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_manifest_artifact_poisoning_is_rejected() {
  local failures=0
  setup_fixture manifest-artifact-poisoning
  local root="$CURRENT_FIXTURE"
  local target=bench_alpha
  local -a cases=(manifest-as-raw shared-artifact nested-artifact raw-symlink report-symlink)
  local -a run_ids=(20260802_121401 20260802_121402 20260802_121403 20260802_121404 20260802_121405)
  write_target "$root" "$target"
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"
  local index case_name run_id directory manifest_file outside

  for index in "${!cases[@]}"; do
    case_name="${cases[$index]}"
    run_id="${run_ids[$index]}"
    directory="$root/benchmark/report/micro/$target/$run_id"
    manifest_file="$directory/manifest.json"
    run_benchmark "$root" micro --run-id "$run_id"
    if ! assert_benchmark_status 0 "$case_name 首次微基准运行"; then failures=$((failures + 1)); fi
    case "$case_name" in
      manifest-as-raw)
        set_manifest_artifact_paths "$manifest_file" manifest.json report.txt
        ;;
      shared-artifact)
        set_manifest_artifact_paths "$manifest_file" raw.txt raw.txt
        ;;
      nested-artifact)
        mkdir -p "$directory/nested"
        printf 'nested raw fixture\n' > "$directory/nested/raw.txt"
        set_manifest_artifact_paths "$manifest_file" nested/raw.txt report.txt
        ;;
      raw-symlink)
        outside="$root/artifact-outside/$run_id-raw.txt"
        mkdir -p "$(dirname "$outside")"
        printf 'outside raw fixture\n' > "$outside"
        rm -f "$directory/raw.txt"
        ln -s "$outside" "$directory/raw.txt"
        ;;
      report-symlink)
        outside="$root/artifact-outside/$run_id-report.txt"
        mkdir -p "$(dirname "$outside")"
        printf 'outside report fixture\n' > "$outside"
        rm -f "$directory/report.txt"
        ln -s "$outside" "$directory/report.txt"
        ;;
    esac
    : > "$root/target.log"
    run_benchmark "$root" micro --resume "$run_id"
    if ! assert_benchmark_status 0 "$case_name 毒化清单续跑"; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$root/target.log" "$case_name 毒化 artifact 必须重跑目标" "$target"; then
      failures=$((failures + 1))
    fi
    if ! assert_manifest_execution_state "$manifest_file" passed 2 0 "$case_name 重跑后的微基准"; then
      failures=$((failures + 1))
    fi
  done

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_non_object_manifests_are_quietly_rejected() {
  local failures=0
  setup_fixture non-object-manifests
  local root="$CURRENT_FIXTURE"
  local target=bench_alpha
  local -a values=('[]' '"not-an-object"' 'null')
  local -a run_ids=(20260802_121501 20260802_121502 20260802_121503)
  write_target "$root" "$target"
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"
  local index value run_id manifest_file output

  for index in "${!values[@]}"; do
    value="${values[$index]}"
    run_id="${run_ids[$index]}"
    manifest_file="$root/benchmark/report/micro/$target/$run_id/manifest.json"
    run_benchmark "$root" micro --run-id "$run_id"
    if ! assert_benchmark_status 0 "非对象 JSON 第 $((index + 1)) 项首次微基准运行"; then failures=$((failures + 1)); fi
    printf '%s\n' "$value" > "$manifest_file"
    : > "$root/target.log"
    run_benchmark "$root" micro --resume "$run_id"
    if ! assert_benchmark_status 0 "非对象 JSON 第 $((index + 1)) 项微基准续跑"; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$root/target.log" "非对象 JSON 第 $((index + 1)) 项必须重跑目标" "$target"; then
      failures=$((failures + 1))
    fi
    output="$(<"$BENCHMARK_LOG")"
    if ! assert_not_contains "$output" Traceback "非对象 JSON 第 $((index + 1)) 项不得输出 Python traceback"; then
      failures=$((failures + 1))
    fi
  done

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_non_utf8_manifests_are_quietly_rejected() {
  local failures=0
  setup_fixture non-utf8-manifest
  local root="$CURRENT_FIXTURE"
  local target=bench_alpha
  local run_id=20260802_121601
  local manifest_file="$root/benchmark/report/micro/$target/$run_id/manifest.json"
  write_target "$root" "$target"

  run_benchmark "$root" micro --run-id "$run_id"
  if ! assert_benchmark_status 0 '非 UTF-8 清单首次微基准运行'; then failures=$((failures + 1)); fi
  printf '\377\376\n' > "$manifest_file"
  : > "$root/target.log"
  run_benchmark "$root" micro --resume "$run_id"
  if ! assert_benchmark_status 0 '非 UTF-8 清单微基准续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" '非 UTF-8 清单必须重跑目标' "$target"; then
    failures=$((failures + 1))
  fi
  if ! assert_not_contains "$(<"$BENCHMARK_LOG")" Traceback '非 UTF-8 清单不得输出 Python traceback'; then
    failures=$((failures + 1))
  fi
  if ! assert_manifest_passed "$manifest_file" '非 UTF-8 清单重跑后'; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_resume_directory_symlinks_are_rejected() {
  local failures=0
  local -a components=(benchmark report type target run)
  local component run_id=20260802_121701 root link_path outside before after

  for component in "${components[@]}"; do
    setup_fixture "resume-directory-link-$component"
    root="$CURRENT_FIXTURE"
    write_target "$root" bench_alpha
    run_benchmark "$root" micro --run-id "$run_id"
    if ! assert_benchmark_status 0 "$component 目录链接夹具首次微基准运行"; then
      failures=$((failures + 1))
    fi
    case "$component" in
      benchmark)
        link_path="$root/benchmark"
        ;;
      report)
        link_path="$root/benchmark/report"
        ;;
      type)
        link_path="$root/benchmark/report/micro"
        ;;
      target)
        link_path="$root/benchmark/report/micro/bench_alpha"
        ;;
      run)
        link_path="$root/benchmark/report/micro/bench_alpha/$run_id"
        ;;
    esac
    outside="$root/outside-$component"
    mv "$link_path" "$outside"
    ln -s "$outside" "$link_path"
    before="$(snapshot_tree "$outside")"
    : > "$root/target.log"
    run_benchmark "$root" micro --resume "$run_id"
    if ! assert_benchmark_status 1 "$component 目录链接续跑必须拒绝"; then
      failures=$((failures + 1))
    fi
    if [ ! -L "$link_path" ]; then
      fail "$component 目录链接续跑不得替换或跟随目录链接"
      failures=$((failures + 1))
    fi
    after="$(snapshot_tree "$outside")"
    if ! assert_equals "$before" "$after" "$component 目录链接续跑不得修改目录外内容"; then
      failures=$((failures + 1))
    fi
    if ! assert_empty_file "$root/target.log" "$component 目录链接续跑不得执行目标"; then
      failures=$((failures + 1))
    fi
  done
  [ "$failures" -eq 0 ]
}

test_resume_artifact_links_are_replaced_safely() {
  local failures=0
  local -a artifacts=(raw.txt report.txt manifest.json)
  local -a link_types=(symlink hardlink directory_symlink)
  local artifact link_type root run_id path outside before after run_counter=0

  for artifact in "${artifacts[@]}"; do
    for link_type in "${link_types[@]}"; do
      setup_fixture "resume-${artifact//./-}-$link_type"
      root="$CURRENT_FIXTURE"
      run_counter=$((run_counter + 1))
      printf -v run_id '20260802_1218%02d' "$run_counter"
      write_target "$root" bench_alpha
      run_benchmark "$root" micro --run-id "$run_id"
      if ! assert_benchmark_status 0 "$artifact $link_type 夹具首次微基准运行"; then
        failures=$((failures + 1))
      fi
      path="$root/benchmark/report/micro/bench_alpha/$run_id/$artifact"
      outside="$root/outside-${artifact//./-}-$link_type"
      if [ "$link_type" = directory_symlink ]; then
        mkdir -p "$outside"
        printf 'outside directory sentinel for %s\n' "$artifact" > "$outside/sentinel"
      elif [ "$artifact" = manifest.json ]; then
        cp "$path" "$outside"
      else
        printf 'outside sentinel for %s %s\n' "$artifact" "$link_type" > "$outside"
      fi
      rm -f "$path"
      if [ "$link_type" = symlink ] || [ "$link_type" = directory_symlink ]; then
        ln -s "$outside" "$path"
      else
        ln "$outside" "$path"
      fi
      before="$(snapshot_path "$outside")"
      : > "$root/target.log"
      run_benchmark "$root" micro --resume "$run_id"
      if ! assert_benchmark_status 0 "$artifact $link_type 安全续跑"; then
        failures=$((failures + 1))
      fi
      if ! assert_target_lines "$root/target.log" "$artifact $link_type 必须重跑目标" bench_alpha; then
        failures=$((failures + 1))
      fi
      after="$(snapshot_path "$outside")"
      if ! assert_equals "$before" "$after" "$artifact $link_type 不得改变目录外哨兵"; then
        failures=$((failures + 1))
      fi
      if ! assert_single_link_regular_file "$path" "$artifact $link_type 重跑后的当前 artifact"; then
        failures=$((failures + 1))
      fi
      if [ "$link_type" = directory_symlink ]; then
        if ! assert_file "$outside/sentinel" "$artifact 目录型链接重跑后的目录外哨兵"; then
          failures=$((failures + 1))
        fi
      elif ! assert_single_link_regular_file "$outside" "$artifact $link_type 重跑后的目录外哨兵"; then
        failures=$((failures + 1))
      fi
      if ! assert_manifest_passed "$root/benchmark/report/micro/bench_alpha/$run_id/manifest.json" \
        "$artifact $link_type 重跑后的 manifest"; then
        failures=$((failures + 1))
      fi
    done
  done
  [ "$failures" -eq 0 ]
}

test_resume_manifest_identity_is_bound_to_current_location() {
  local failures=0
  local root target run_id manifest_file directory
  local -a micro_fields=(target run_id)
  local -a micro_values=(bench_wrong 20260802_122099)
  local -a micro_run_ids=(20260802_122001 20260802_122002)
  local index field value

  setup_fixture resume-manifest-identity
  root="$CURRENT_FIXTURE"
  target=bench_alpha
  write_target "$root" "$target"
  write_target "$root" qps_alpha
  write_qps_source "$root" qps_alpha

  for index in "${!micro_fields[@]}"; do
    field="${micro_fields[$index]}"
    value="${micro_values[$index]}"
    run_id="${micro_run_ids[$index]}"
    directory="$root/benchmark/report/micro/$target/$run_id"
    manifest_file="$directory/manifest.json"
    run_benchmark "$root" micro --run-id "$run_id"
    if ! assert_benchmark_status 0 "$field 错误清单首次微基准运行"; then failures=$((failures + 1)); fi
    if ! set_manifest_string_field "$manifest_file" "$field" "$value"; then
      fail "无法构造微基准 $field 错误清单"
      failures=$((failures + 1))
    fi
    : > "$root/target.log"
    run_benchmark "$root" micro --resume "$run_id"
    if ! assert_benchmark_status 0 "微基准 $field 错误清单续跑"; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$root/target.log" "微基准 $field 错误清单不得进入 resume" "$target"; then
      failures=$((failures + 1))
    fi
    if ! assert_manifest_contract "$manifest_file" micro "$target" default "$run_id" "微基准 $field 重跑后"; then
      failures=$((failures + 1))
    fi
  done

  run_id=20260802_122003
  directory="$root/benchmark/report/micro/$target/$run_id"
  run_benchmark "$root" micro --run-id "$run_id"
  if ! assert_benchmark_status 0 '错误物理目标目录夹具首次微基准运行'; then failures=$((failures + 1)); fi
  mkdir -p "$root/benchmark/report/micro/bench_wrong"
  mv "$directory" "$root/benchmark/report/micro/bench_wrong/$run_id"
  : > "$root/target.log"
  run_benchmark "$root" micro --resume "$run_id"
  if ! assert_benchmark_status 0 '错误物理目标目录的微基准续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" '错误物理目标目录清单不得进入 resume' "$target"; then
    failures=$((failures + 1))
  fi
  if ! assert_manifest_contract "$directory/manifest.json" micro "$target" default "$run_id" \
    '错误物理目标目录重跑后的微基准'; then
    failures=$((failures + 1))
  fi

  run_id=20260802_122004
  directory="$root/benchmark/report/qps/qps_alpha/$run_id"
  manifest_file="$directory/manifest.json"
  run_benchmark "$root" qps smoke --run-id "$run_id"
  if ! assert_benchmark_status 0 '错误 profile 清单首次 QPS 运行'; then failures=$((failures + 1)); fi
  if ! set_manifest_string_field "$manifest_file" profile full; then
    fail '无法构造 QPS profile 错误清单'
    failures=$((failures + 1))
  fi
  : > "$root/target.log"
  run_benchmark "$root" qps smoke --resume "$run_id"
  if ! assert_benchmark_status 0 'QPS profile 错误清单续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" 'QPS profile 错误清单不得进入 resume' qps_alpha; then
    failures=$((failures + 1))
  fi
  if ! assert_manifest_contract "$manifest_file" qps qps_alpha smoke "$run_id" 'QPS profile 重跑后'; then
    failures=$((failures + 1))
  fi
  [ "$failures" -eq 0 ]
}

test_run_directory_replacement_does_not_publish_artifacts_outside() {
  local failures=0
  setup_fixture run-directory-replacement
  local root="$CURRENT_FIXTURE"
  local target=bench_alpha
  local run_id=20260802_122101
  local run_dir="$root/benchmark/report/micro/$target/$run_id"
  local moved_dir="$root/moved-run-directory"
  local sentinel_dir="$root/outside-artifact-sentinel"
  local before after artifact
  write_target "$root" "$target"

  run_benchmark "$root" micro --run-id "$run_id"
  if ! assert_benchmark_status 0 '目录替换夹具首次微基准运行'; then failures=$((failures + 1)); fi
  printf '# force a resume rerun\n' >> "$root/bin/$target"
  mkdir -p "$sentinel_dir"
  for artifact in raw.txt report.txt manifest.json; do
    printf 'outside sentinel: %s\n' "$artifact" > "$sentinel_dir/$artifact"
  done
  before="$(snapshot_tree "$sentinel_dir")"

  : > "$root/target.log"
  BENCHMARK_ENV=(
    "FAKE_SWAP_RUN_DIR=$run_dir"
    "FAKE_SWAP_MOVED_DIR=$moved_dir"
    "FAKE_SWAP_LINK_TARGET=$sentinel_dir"
  )
  run_benchmark "$root" micro --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 '运行目录被替换后必须拒绝发布'; then failures=$((failures + 1)); fi
  if [ ! -L "$run_dir" ]; then
    fail '运行目录被替换后不得覆盖目录链接'
    failures=$((failures + 1))
  fi
  after="$(snapshot_tree "$sentinel_dir")"
  if ! assert_equals "$before" "$after" '运行目录被替换后不得写入目录外哨兵'; then
    failures=$((failures + 1))
  fi
  if ! assert_target_lines "$root/target.log" '运行目录替换前应只执行一次目标' "$target"; then
    failures=$((failures + 1))
  fi

  rm -f "$run_dir"
  mv "$moved_dir" "$run_dir"
  : > "$root/target.log"
  run_benchmark "$root" micro --resume "$run_id"
  if ! assert_benchmark_status 0 '恢复安全目录后的微基准续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" '恢复安全目录后必须重跑目标' "$target"; then
    failures=$((failures + 1))
  fi
  for artifact in raw.txt report.txt manifest.json; do
    if ! assert_single_link_regular_file "$run_dir/$artifact" "恢复安全目录后的 $artifact"; then
      failures=$((failures + 1))
    fi
  done
  if ! assert_manifest_passed "$run_dir/manifest.json" '恢复安全目录后的 manifest'; then
    failures=$((failures + 1))
  fi
  after="$(snapshot_tree "$sentinel_dir")"
  if ! assert_equals "$before" "$after" '恢复安全目录后不得改变目录外哨兵'; then
    failures=$((failures + 1))
  fi
  [ "$failures" -eq 0 ]
}

test_qps_resume_all_incomplete_states() {
  local failures=0
  setup_fixture qps-resume-incomplete-states
  local root="$CURRENT_FIXTURE"
  local target=qps_alpha
  write_target "$root" "$target"
  write_qps_source "$root" "$target"
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"

  BENCHMARK_ENV=("FAKE_FAIL_ONCE_TARGETS=$target")
  run_benchmark "$root" qps smoke --run-id 20260802_121201
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 'QPS failed 首次运行'; then failures=$((failures + 1)); fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121201/manifest.json" \
    failed 1 41 'QPS failed 首次运行'; then
    failures=$((failures + 1))
  fi
  : > "$root/target.log"
  run_benchmark "$root" qps smoke --resume 20260802_121201
  if ! assert_benchmark_status 0 'QPS failed 续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" 'QPS failed 清单必须重跑' "$target"; then failures=$((failures + 1)); fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121201/manifest.json" \
    passed 2 0 'QPS failed 续跑'; then
    failures=$((failures + 1))
  fi

  BENCHMARK_ENV=("FAKE_TIMEOUT_ONCE_TARGETS=$target")
  run_benchmark "$root" qps smoke --run-id 20260802_121202
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 'QPS timeout 首次运行'; then failures=$((failures + 1)); fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121202/manifest.json" \
    timeout 1 124 'QPS timeout 首次运行'; then
    failures=$((failures + 1))
  fi
  : > "$root/target.log"
  run_benchmark "$root" qps smoke --resume 20260802_121202
  if ! assert_benchmark_status 0 'QPS timeout 续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" 'QPS timeout 清单必须重跑' "$target"; then failures=$((failures + 1)); fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121202/manifest.json" \
    passed 2 0 'QPS timeout 续跑'; then
    failures=$((failures + 1))
  fi

  run_benchmark "$root" qps smoke --run-id 20260802_121203
  if ! assert_benchmark_status 0 'QPS running 首次运行'; then failures=$((failures + 1)); fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121203/manifest.json" \
    passed 1 0 'QPS running 夹具构造前'; then
    failures=$((failures + 1))
  fi
  if ! /usr/bin/python3 - "$root/benchmark/report/qps/$target/20260802_121203/manifest.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    manifest = json.load(stream)
manifest["state"] = "running"
manifest["status"] = "running"
manifest["exit_code"] = None
manifest["elapsed_seconds"] = None
manifest["result"]["status"] = "running"
manifest["result"]["exit_code"] = None
manifest["result"]["elapsed_seconds"] = None
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
  then
    fail '无法构造 QPS running 清单夹具'
    failures=$((failures + 1))
  fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121203/manifest.json" \
    running 1 null 'QPS running 清单夹具'; then
    failures=$((failures + 1))
  fi
  : > "$root/target.log"
  run_benchmark "$root" qps smoke --resume 20260802_121203
  if ! assert_benchmark_status 0 'QPS running 续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" 'QPS running 清单必须重跑' "$target"; then failures=$((failures + 1)); fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121203/manifest.json" \
    passed 2 0 'QPS running 续跑'; then
    failures=$((failures + 1))
  fi

  run_benchmark "$root" qps smoke --run-id 20260802_121204
  if ! assert_benchmark_status 0 'QPS 损坏清单首次运行'; then failures=$((failures + 1)); fi
  printf '{damaged manifest\n' > "$root/benchmark/report/qps/$target/20260802_121204/manifest.json"
  : > "$root/target.log"
  run_benchmark "$root" qps smoke --resume 20260802_121204
  if ! assert_benchmark_status 0 'QPS 损坏清单续跑'; then failures=$((failures + 1)); fi
  if ! assert_target_lines "$root/target.log" 'QPS 损坏清单必须重跑' "$target"; then failures=$((failures + 1)); fi

  run_benchmark "$root" qps smoke --run-id 20260802_121205
  if ! assert_benchmark_status 0 'QPS passed 首次运行'; then failures=$((failures + 1)); fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121205/manifest.json" \
    passed 1 0 'QPS passed 首次运行'; then
    failures=$((failures + 1))
  fi
  : > "$root/target.log"
  run_benchmark "$root" qps smoke --resume 20260802_121205
  if ! assert_benchmark_status 0 'QPS passed 续跑'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/target.log" '完整同指纹 QPS passed 清单必须跳过'; then
    failures=$((failures + 1))
  fi
  if ! assert_manifest_execution_state "$root/benchmark/report/qps/$target/20260802_121205/manifest.json" \
    passed 1 0 'QPS passed 续跑'; then
    failures=$((failures + 1))
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_target_level_layout() {
  local root="$TEST_TMP_DIR/target-level"
  local failures=0
  setup_fixture target-level
  root="$CURRENT_FIXTURE"
  write_target "$root" bench_alpha
  write_target "$root" qps_alpha
  write_qps_source "$root" qps_alpha
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"

  run_benchmark "$root" micro
  if ! assert_benchmark_status 0 '微基准伪目标运行'; then failures=$((failures + 1)); fi
  if ! discover_single_run_id "$root/benchmark/report/micro/bench_alpha" '微基准 bench_alpha'; then
    failures=$((failures + 1))
  fi

  run_benchmark "$root" qps full
  if ! assert_benchmark_status 0 'QPS 伪目标运行'; then failures=$((failures + 1)); fi
  if ! discover_single_run_id "$root/benchmark/report/qps/qps_alpha" 'QPS qps_alpha'; then
    failures=$((failures + 1))
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_resume_failed_target_only() {
  local failures=0
  setup_fixture resume-failed-target
  local root="$CURRENT_FIXTURE"
  write_target "$root" bench_alpha
  write_target "$root" bench_beta
  write_legacy_report "$root" micro 20990101_000001 LEGACY_RESUME_ALL_PASSED
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"

  BENCHMARK_ENV=(FAKE_FAIL_ONCE_TARGETS=bench_beta)
  run_benchmark "$root" micro
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 '首次微基准应保留 beta 失败状态'; then failures=$((failures + 1)); fi

  local alpha_id=""
  local beta_id=""
  if alpha_id=$(discover_single_run_id "$root/benchmark/report/micro/bench_alpha" '续跑前 bench_alpha'); then
    :
  else
    failures=$((failures + 1))
  fi
  if beta_id=$(discover_single_run_id "$root/benchmark/report/micro/bench_beta" '续跑前 bench_beta'); then
    :
  else
    failures=$((failures + 1))
  fi
  if [ -n "$alpha_id" ] && [ -n "$beta_id" ]; then
    if ! assert_equals "$alpha_id" "$beta_id" '同一次微基准运行的目标应共享 run-id'; then failures=$((failures + 1)); fi
    : > "$root/target.log"
    run_benchmark "$root" micro --resume "$alpha_id"
    if ! assert_benchmark_status 0 '相同指纹 --resume 应仅补跑失败目标后成功'; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$root/target.log" '相同指纹 --resume 不应重跑已通过目标' bench_beta; then
      failures=$((failures + 1))
    fi
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_fingerprint_changes_rerun_passed_targets() {
  local failures=0
  setup_fixture fingerprint-micro
  local micro_root="$CURRENT_FIXTURE"
  write_target "$micro_root" bench_alpha
  local micro_legacy_before
  micro_legacy_before="$(snapshot_tree "$micro_root/benchmark/reports")"
  run_benchmark "$micro_root" micro
  if ! assert_benchmark_status 0 '指纹微基准首次运行'; then failures=$((failures + 1)); fi
  local micro_id=""
  if micro_id=$(discover_single_run_id "$micro_root/benchmark/report/micro/bench_alpha" '微基准指纹'); then
    printf '# binary changed by regression fixture\n' >> "$micro_root/bin/bench_alpha"
    : > "$micro_root/target.log"
    run_benchmark "$micro_root" micro --resume "$micro_id"
    if ! assert_benchmark_status 0 '伪二进制变化后 --resume 应成功'; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$micro_root/target.log" '伪二进制变化后应重跑已通过目标' bench_alpha; then
      failures=$((failures + 1))
    fi

    : > "$micro_root/target.log"
    BENCHMARK_ENV=(BENCH_FLAGS=--benchmark_min_time=0.2s)
    run_benchmark "$micro_root" micro --resume "$micro_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 0 'BENCH_FLAGS 变化后 --resume 应成功'; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$micro_root/target.log" 'BENCH_FLAGS 变化后应重跑已通过目标' bench_alpha; then
      failures=$((failures + 1))
    fi
  else
    failures=$((failures + 1))
  fi
  if ! assert_legacy_not_written "$micro_root" "$micro_legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$micro_root"

  setup_fixture fingerprint-qps
  local qps_root="$CURRENT_FIXTURE"
  write_target "$qps_root" qps_alpha
  write_qps_source "$qps_root" qps_alpha
  write_legacy_report "$qps_root" qps 20990101_000001 LEGACY_QPS_RESUME_ALL_PASSED
  local qps_legacy_before
  qps_legacy_before="$(snapshot_tree "$qps_root/benchmark/reports")"
  run_benchmark "$qps_root" qps smoke
  if ! assert_benchmark_status 0 '指纹 QPS 首次运行'; then failures=$((failures + 1)); fi
  local qps_id=""
  if qps_id=$(discover_single_run_id "$qps_root/benchmark/report/qps/qps_alpha" 'QPS 指纹'); then
    : > "$qps_root/target.log"
    BENCHMARK_ENV=(QPS_PROFILE=full)
    run_benchmark "$qps_root" qps --resume "$qps_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 0 'QPS_PROFILE 变化后 --resume 应成功'; then failures=$((failures + 1)); fi
    if ! assert_target_lines "$qps_root/target.log" 'QPS_PROFILE 变化后应重跑已通过目标' qps_alpha; then
      failures=$((failures + 1))
    fi
  else
    failures=$((failures + 1))
  fi
  if ! assert_legacy_not_written "$qps_root" "$qps_legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$qps_root"
  if ! assert_no_docker "$qps_root" || ! assert_no_docker "$micro_root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_resume_and_fingerprint() {
  local failures=0
  setup_fixture rps-resume
  local root="$CURRENT_FIXTURE"
  local failed_cell=005_upload_1kb_c1_r1
  local refreshed_fingerprint=fixture-rps-v2
  local -a baseline_cells=(
    001_health_c1_r1
    002_auth_me_c1_r1
    003_files_list_c1_r1
    004_music_list_c1_r1
    005_upload_1kb_c1_r1
    006_upload_1mb_c1_r1
    007_download_1kb_c1_r1
    008_download_1mb_c1_r1
    009_range_1mb_c1_r1
  )
  local -a changed_matrix_cells=(
    001_health_c1_r1
    002_health_c2_r1
    003_auth_me_c1_r1
    004_auth_me_c2_r1
    005_files_list_c1_r1
    006_files_list_c2_r1
    007_music_list_c1_r1
    008_music_list_c2_r1
    009_upload_1kb_c1_r1
    010_upload_1mb_c1_r1
    011_download_1kb_c1_r1
    012_download_1mb_c1_r1
    013_range_1mb_c1_r1
  )
  write_legacy_report "$root" rps 20990101_000001 LEGACY_RPS_RESUME_ALL_PASSED
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"
  configure_rps_environment 1 fixture-rps-v1
  BENCHMARK_ENV+=("FAKE_WRK_FAIL_CELL=$failed_cell")
  run_benchmark "$root" rps full
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 'RPS 首次运行应保留单元失败状态'; then failures=$((failures + 1)); fi
  if ! assert_recorded_lines "$root/wrk.log" 'RPS 首次受控 full 矩阵必须处理全部预期 cell 身份' "${baseline_cells[@]}"; then
    failures=$((failures + 1))
  fi

  local rps_id=""
  if rps_id=$(discover_single_run_id "$root/benchmark/report/rps/full" 'RPS 单元续跑'); then
    : > "$root/wrk.log"
    configure_rps_environment 1 fixture-rps-v1
    run_benchmark "$root" rps full --resume "$rps_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 0 'RPS 相同指纹 --resume 应成功'; then failures=$((failures + 1)); fi
    if ! assert_recorded_lines "$root/wrk.log" 'RPS 相同指纹 --resume 只能执行首次失败的实际 cell' "$failed_cell"; then
      failures=$((failures + 1))
    fi

    : > "$root/wrk.log"
    configure_rps_environment 1 "$refreshed_fingerprint"
    run_benchmark "$root" rps full --resume "$rps_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 0 'RPS_TARGET_FINGERPRINT 变化后 --resume 应成功'; then failures=$((failures + 1)); fi
    if ! assert_recorded_lines "$root/wrk.log" 'RPS_TARGET_FINGERPRINT 变化后应重跑 full 矩阵全部实际 cell' "${baseline_cells[@]}"; then
      failures=$((failures + 1))
    fi

    : > "$root/wrk.log"
    configure_rps_environment 1 "$refreshed_fingerprint"
    run_benchmark "$root" rps full --resume "$rps_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 0 'RPS 目标指纹已匹配时 --resume 应成功'; then failures=$((failures + 1)); fi
    if ! assert_empty_file "$root/wrk.log" 'RPS 目标指纹与矩阵参数均匹配时不应重跑任何 cell'; then
      failures=$((failures + 1))
    fi

    : > "$root/wrk.log"
    configure_rps_environment '1 2' "$refreshed_fingerprint"
    run_benchmark "$root" rps full --resume "$rps_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 0 'RPS 并发矩阵参数变化后 --resume 应成功'; then failures=$((failures + 1)); fi
    if ! assert_recorded_lines "$root/wrk.log" 'RPS 并发矩阵参数变化后必须重跑全部受控实际 cell' "${changed_matrix_cells[@]}"; then
      failures=$((failures + 1))
    fi
  else
    failures=$((failures + 1))
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_resume_reports_rebuild_complete_matrix() {
  local failures=0
  local root run_id=20260803_100001 failed_cell=005_upload_1kb_c1_r1
  local report_json report_text artifact_text

  setup_fixture rps-report-resume
  root="$CURRENT_FIXTURE"
  report_json="$root/benchmark/report/rps/full/$run_id/report.json"
  report_text="$root/benchmark/report/rps/full/$run_id/report.txt"
  configure_rps_environment 1 fixture-rps-report
  BENCHMARK_ENV+=("FAKE_WRK_FAIL_CELL=$failed_cell")
  run_benchmark "$root" rps full --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 'RPS 首次单元失败必须写入完整失败报告'; then failures=$((failures + 1)); fi
  if ! /usr/bin/python3 - "$report_json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
summary = data["summary"]
assert summary["expected_cells"] == 9
assert summary["completed_cells"] == 9
assert summary["failed_cells"] == 1
assert summary["result"] == "failed"
assert len(data["results"]) == 9
PY
  then
    fail '首次失败 RPS 报告必须包含全部矩阵单元并标记失败'
    failures=$((failures + 1))
  fi

  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-report
  run_benchmark "$root" rps full --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 'RPS 续跑失败单元后必须通过'; then failures=$((failures + 1)); fi
  if ! assert_recorded_lines "$root/wrk.log" 'RPS 续跑只应补跑首次失败单元' "$failed_cell"; then failures=$((failures + 1)); fi
  if ! /usr/bin/python3 - "$report_json" "$report_text" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
summary = data["summary"]
assert summary["expected_cells"] == 9
assert summary["completed_cells"] == 9
assert summary["passed_cells"] == 9
assert summary["failed_cells"] == 0
assert summary["result"] == "passed"
assert len(data["results"]) == 9
with open(sys.argv[2], encoding="utf-8") as stream:
    assert "矩阵: 9/9" in stream.read()
PY
  then
    fail '补跑后的 RPS JSON 与文本报告必须重建完整通过矩阵'
    failures=$((failures + 1))
  fi

  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-report
  run_benchmark "$root" rps full --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '全部 RPS 单元跳过时必须保留通过结果'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/wrk.log" '全部完整 RPS 单元在 resume 时不得重跑'; then failures=$((failures + 1)); fi
  if ! /usr/bin/python3 - "$report_json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
summary = data["summary"]
assert summary["completed_cells"] == 9
assert summary["failed_cells"] == 0
assert summary["result"] == "passed"
assert len(data["results"]) == 9
PY
  then
    fail '全部跳过后的 RPS 报告不得退化为空矩阵'
    failures=$((failures + 1))
  fi
  artifact_text="$(cat "$report_json" "$report_text" "$root"/benchmark/report/rps/full/"$run_id"/raw/*.txt)"
  for secret in fake-token RPS_AUTH_TOKEN password Pass! fixture-rps-report hps_rps_; do
    if ! assert_not_contains "$artifact_text" "$secret" "RPS report/raw artifact 不得泄露敏感或临时值"; then failures=$((failures + 1)); fi
  done
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_corrupt_cells_are_rerun() {
  local failures=0
  local root run_id=20260803_100002 manifest first_cell=001_health_c1_r1 second_cell=002_auth_me_c1_r1
  local third_cell=003_files_list_c1_r1

  setup_fixture rps-corrupt-cell
  root="$CURRENT_FIXTURE"
  manifest="$root/benchmark/report/rps/full/$run_id/manifest.json"
  configure_rps_environment 1 fixture-rps-corrupt
  run_benchmark "$root" rps full --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 'RPS 损坏 cell 夹具首次运行'; then failures=$((failures + 1)); fi

  /usr/bin/python3 - "$manifest" "$first_cell" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
data["cells"][sys.argv[2]] = {"state": "passed"}
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(data, stream)
PY
  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-corrupt
  run_benchmark "$root" rps full --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '缺失 RPS cell 完整字段时必须补跑'; then failures=$((failures + 1)); fi
  if ! assert_recorded_lines "$root/wrk.log" '不完整 passed cell 必须被重新执行' "$first_cell"; then failures=$((failures + 1)); fi

  /usr/bin/python3 - "$manifest" "$second_cell" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
cell = data["cells"][sys.argv[2]]
cell["attempt"] = True
cell["exit_code"] = "0"
cell["started_at"] = 1
cell["finished_at"] = []
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(data, stream)
PY
  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-corrupt
  run_benchmark "$root" rps full --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '类型错误的 RPS cell 字段时必须补跑'; then failures=$((failures + 1)); fi
  if ! assert_recorded_lines "$root/wrk.log" '类型错误 passed cell 必须被重新执行' "$second_cell"; then failures=$((failures + 1)); fi

  /usr/bin/python3 - "$manifest" "$third_cell" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
data["cells"][sys.argv[2]]["record"]["rps"] = float("nan")
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(data, stream)
PY
  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-corrupt
  run_benchmark "$root" rps full --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '非有限 RPS cell 字段时必须补跑'; then failures=$((failures + 1)); fi
  if ! assert_recorded_lines "$root/wrk.log" '非有限 passed cell 必须被重新执行' "$third_cell"; then failures=$((failures + 1)); fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_extra_manifest_cells_are_pruned_on_resume() {
  local failures=0
  local root run_id=20260803_100004 run_dir manifest report_json report_text

  setup_fixture rps-extra-manifest-cell
  root="$CURRENT_FIXTURE"
  run_dir="$root/benchmark/report/rps/full/$run_id"
  manifest="$run_dir/manifest.json"
  report_json="$run_dir/report.json"
  report_text="$run_dir/report.txt"
  configure_rps_environment 1 fixture-rps-extra-cell
  run_benchmark "$root" rps full --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 'RPS extra cell 夹具首次运行'; then failures=$((failures + 1)); fi

  if ! /usr/bin/python3 - "$manifest" "$run_dir/raw/extra_cell.txt" <<'PY'
import copy
import json
import sys

manifest_file, raw_file = sys.argv[1:]
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)
extra = copy.deepcopy(manifest["cells"]["001_health_c1_r1"])
extra["record"]["index"] = 10
extra["record"]["raw_file"] = "raw/extra_cell.txt"
manifest["cells"]["extra_cell"] = extra
with open(raw_file, "w", encoding="utf-8") as stream:
    stream.write("stale extra RPS cell\\n")
with open(manifest_file, "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
  then
    fail '注入额外 RPS manifest cell 失败'
    failures=$((failures + 1))
  fi

  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-extra-cell
  run_benchmark "$root" rps full --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '同指纹 RPS resume 必须清除额外 manifest cell'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/wrk.log" '额外 manifest cell 不得导致有效 RPS 单元重跑'; then failures=$((failures + 1)); fi
  if ! /usr/bin/python3 - "$manifest" "$report_json" "$report_text" <<'PY'
import json
import sys

manifest_file, report_json, report_text = sys.argv[1:]
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)
expected = {
    "001_health_c1_r1",
    "002_auth_me_c1_r1",
    "003_files_list_c1_r1",
    "004_music_list_c1_r1",
    "005_upload_1kb_c1_r1",
    "006_upload_1mb_c1_r1",
    "007_download_1kb_c1_r1",
    "008_download_1mb_c1_r1",
    "009_range_1mb_c1_r1",
}
assert set(manifest["cells"]) == expected
with open(report_json, encoding="utf-8") as stream:
    report = json.load(stream)
assert len(report["results"]) == len(expected)
assert all(row["raw_file"] != "raw/extra_cell.txt" for row in report["results"])
with open(report_text, encoding="utf-8") as stream:
    assert "extra_cell" not in stream.read()
PY
  then
    fail 'RPS terminal manifest 和报告必须排除额外 cell'
    failures=$((failures + 1))
  fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_resume_artifacts_replace_links_without_following_them() {
  local failures=0
  local root run_id=20260803_100003 run_dir raw_cell=001_health_c1_r1 sentinel sentinel_before

  setup_fixture rps-artifact-links
  root="$CURRENT_FIXTURE"
  run_dir="$root/benchmark/report/rps/full/$run_id"
  configure_rps_environment 1 fixture-rps-artifacts
  run_benchmark "$root" rps full --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 'RPS artifact link 夹具首次运行'; then failures=$((failures + 1)); fi
  sentinel="$root/outside-sentinel.txt"
  printf 'outside sentinel must remain unchanged\n' > "$sentinel"
  sentinel_before="$(snapshot_path "$sentinel")"
  rm "$run_dir/raw/$raw_cell.txt" "$run_dir/report.json"
  ln -s "$sentinel" "$run_dir/raw/$raw_cell.txt"
  ln -s "$sentinel" "$run_dir/report.json"

  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-artifacts
  run_benchmark "$root" rps full --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 'RPS resume 必须安全重建被链接的 artifact'; then failures=$((failures + 1)); fi
  if ! assert_recorded_lines "$root/wrk.log" '被链接的 raw cell 必须被重跑' "$raw_cell"; then failures=$((failures + 1)); fi
  if ! assert_equals "$sentinel_before" "$(snapshot_path "$sentinel")" 'RPS artifact resume 不得写入目录外 sentinel'; then failures=$((failures + 1)); fi
  if ! assert_single_link_regular_file "$run_dir/raw/$raw_cell.txt" 'RPS raw cell 重建'; then failures=$((failures + 1)); fi
  if ! assert_single_link_regular_file "$run_dir/report.json" 'RPS JSON report 重建'; then failures=$((failures + 1)); fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_timeout_is_reported_as_failed() {
  local failures=0
  local root run_id=20260803_100004 report_json manifest timeout_cell=001_health_c1_r1

  setup_fixture rps-timeout-report
  root="$CURRENT_FIXTURE"
  report_json="$root/benchmark/report/rps/full/$run_id/report.json"
  manifest="$root/benchmark/report/rps/full/$run_id/manifest.json"
  configure_rps_environment 1 fixture-rps-timeout
  BENCHMARK_ENV+=("FAKE_WRK_TIMEOUT_CELL=$timeout_cell")
  run_benchmark "$root" rps full --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 'RPS timeout 必须返回失败'; then failures=$((failures + 1)); fi
  if ! /usr/bin/python3 - "$report_json" "$manifest" "$timeout_cell" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    report = json.load(stream)
summary = report["summary"]
assert summary["failed_cells"] == 1
assert summary["result"] == "failed"
assert any(row["result"] == "timeout" for row in report["results"])
with open(sys.argv[2], encoding="utf-8") as stream:
    manifest = json.load(stream)
assert manifest["state"] == "failed"
assert manifest["exit_code"] == 1
assert manifest["cells"][sys.argv[3]]["state"] == "timeout"
PY
  then
    fail 'RPS timeout 的 report、manifest 和退出码必须一致为失败'
    failures=$((failures + 1))
  fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_profiles_and_managed_lifecycle() {
  local failures=0
  local root manifest_text

  setup_fixture rps-managed-profiles
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-managed
  BENCHMARK_ENV+=(RPS_BASE_URL=)
  run_benchmark "$root" rps full overload --run-id 20260802_123000
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '托管 RPS full 与 overload 应共享一次环境并成功'; then failures=$((failures + 1)); fi
  for operation in deploy base-url runtime-fingerprint down; do
    if ! assert_docker_operation_count "$root" "$operation" 1 "托管多 profile 的 $operation 必须恰好一次"; then
      failures=$((failures + 1))
    fi
  done
  if ! assert_file "$root/benchmark/report/rps/full/20260802_123000/manifest.json" 'full RPS 清单'; then
    failures=$((failures + 1))
  fi
  if ! assert_file "$root/benchmark/report/rps/overload/20260802_123000/manifest.json" 'overload RPS 清单'; then
    failures=$((failures + 1))
  fi
  if ! awk '$0 != "http://managed.fake" { exit 1 } END { exit NR == 0 }' "$root/wrk-url.log"; then
    fail '多 profile 的 wrk 请求必须共用托管 base URL'
    failures=$((failures + 1))
  fi
  manifest_text="$(cat "$root/benchmark/report/rps/full/20260802_123000/manifest.json" 2>/dev/null || true)"
  for secret in fake-token Isolated_ ADMIN_PASSWORD hps_rps_; do
    if ! assert_not_contains "$manifest_text" "$secret" "托管 RPS manifest 不得泄露敏感值或项目名"; then
      failures=$((failures + 1))
    fi
  done
  for secret in fake-token Isolated_ ADMIN_PASSWORD; do
    if ! assert_not_contains "$(cat "$root/tools.log")" "$secret" "RPS curl 命令参数不得泄露 $secret"; then
      failures=$((failures + 1))
    fi
    if ! assert_not_contains "$(cat "$root/python.log")" "$secret" "RPS Python 命令参数不得泄露 $secret"; then
      failures=$((failures + 1))
    fi
  done

  setup_fixture rps-external-url
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-external
  run_benchmark "$root" rps full --run-id 20260802_123001
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 '显式 RPS_BASE_URL 与目标指纹应成功'; then failures=$((failures + 1)); fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi

  setup_fixture rps-external-missing-fingerprint
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-unused
  BENCHMARK_ENV+=(RPS_TARGET_FINGERPRINT=)
  run_benchmark "$root" rps full --run-id 20260802_123002
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 2 '显式 RPS_BASE_URL 未给目标指纹必须前置失败'; then failures=$((failures + 1)); fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/wrk.log" '缺少显式目标指纹时不得执行 RPS 单元'; then failures=$((failures + 1)); fi

  setup_fixture rps-managed-cell-failure
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-cell-failure
  BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_WRK_FAIL_CELL=005_upload_1kb_c1_r1)
  run_benchmark "$root" rps full --run-id 20260802_123003
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 '托管 RPS 单元失败必须保留业务失败码'; then failures=$((failures + 1)); fi
  if ! assert_docker_operation_count "$root" down 1 '单元失败后托管环境必须恰好 down 一次'; then failures=$((failures + 1)); fi

  setup_fixture rps-managed-deploy-failure
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-deploy-failure
  BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_DOCKER_DEPLOY_FAIL=1)
  run_benchmark "$root" rps full --run-id 20260802_123004
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 71 '托管 deploy 失败必须保留 deploy 退出码'; then failures=$((failures + 1)); fi
  if ! assert_docker_operation_count "$root" down 1 'deploy 失败后 helper 清理必须恰好 down 一次'; then failures=$((failures + 1)); fi

  setup_fixture rps-managed-base-url-failure
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-base-url-failure
  BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_DOCKER_BASE_URL_FAIL=1)
  run_benchmark "$root" rps full --run-id 20260802_123006
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 72 '托管 base-url 失败必须保留 base-url 退出码'; then failures=$((failures + 1)); fi
  if ! assert_docker_operation_count "$root" down 1 'base-url 失败后 helper 清理必须恰好 down 一次'; then failures=$((failures + 1)); fi

  setup_fixture rps-managed-cleanup-failure
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-cleanup-failure
  BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_DOCKER_DOWN_FAIL=1)
  run_benchmark "$root" rps full --run-id 20260802_123005
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 68 '仅 cleanup 失败时必须返回 cleanup 退出码'; then failures=$((failures + 1)); fi
  if ! assert_docker_operation_count "$root" down 1 'cleanup 失败也不得重复 down'; then failures=$((failures + 1)); fi

  [ "$failures" -eq 0 ]
}

test_rps_managed_startup_failures_write_profile_reports() {
  local failures=0
  local root run_id expected_rc failure_mode

  for failure_mode in deploy base-url fingerprint empty-fingerprint; do
    setup_fixture "rps-managed-$failure_mode-report"
    root="$CURRENT_FIXTURE"
    case "$failure_mode" in
      deploy)
        run_id=20260803_110001
        expected_rc=71
        configure_rps_environment 1 fixture-rps-startup
        BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_DOCKER_DEPLOY_FAIL=1)
        ;;
      base-url)
        run_id=20260803_110004
        expected_rc=72
        configure_rps_environment 1 fixture-rps-startup
        BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_DOCKER_BASE_URL_FAIL=1)
        ;;
      fingerprint)
        run_id=20260803_110002
        expected_rc=73
        configure_rps_environment 1 fixture-rps-startup
        BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_DOCKER_RUNTIME_FINGERPRINT_FAIL=1)
        ;;
      empty-fingerprint)
        run_id=20260803_110003
        expected_rc=1
        configure_rps_environment 1 fixture-rps-startup
        BENCHMARK_ENV+=(RPS_BASE_URL= FAKE_DOCKER_RUNTIME_FINGERPRINT=)
        ;;
    esac
    BENCHMARK_ENV+=(
      "FAKE_DOCKER_REPORT_ROOT=$root/benchmark/report/rps"
      "FAKE_DOCKER_REPORT_RUN_ID=$run_id"
      'FAKE_DOCKER_REPORT_PROFILES=full overload'
      "FAKE_DOCKER_ORDER_LOG=$root/docker-order.log"
    )
    run_benchmark "$root" rps full overload --run-id "$run_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status "$expected_rc" "托管 $failure_mode 失败必须保留原退出码"; then failures=$((failures + 1)); fi
    for profile in full overload; do
      if ! assert_rps_failed_profile_artifacts "$root" "$profile" "$run_id" "$expected_rc" \
        "托管 $failure_mode 失败的 $profile"; then failures=$((failures + 1)); fi
    done
    if ! assert_docker_operation_count "$root" down 1 "托管 $failure_mode 后必须恰好 down 一次"; then failures=$((failures + 1)); fi
    if ! assert_equals 'down-reports-ready' "$(cat "$root/docker-order.log")" \
      "托管 $failure_mode 必须在 down 前发布所有 profile 报告"; then
      failures=$((failures + 1))
    fi
  done
  [ "$failures" -eq 0 ]
}

test_rps_invalid_rps_metrics_are_failed() {
  local failures=0
  local root run_id=20260803_120001

  setup_fixture rps-invalid-rps
  root="$CURRENT_FIXTURE"
  configure_rps_environment 1 fixture-rps-invalid
  BENCHMARK_ENV+=('FAKE_WRK_RPS_BY_CELL=001_health_c1_r1=abc 002_auth_me_c1_r1=0 003_files_list_c1_r1=-1 004_music_list_c1_r1=NaN 005_upload_1kb_c1_r1=inf')
  run_benchmark "$root" rps full --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 1 '非法 RPS 指标必须作为失败处理'; then failures=$((failures + 1)); fi
  if ! /usr/bin/python3 - "$root/benchmark/report/rps/full/$run_id" <<'PY'
import json
import math
import os
import sys

run_dir = sys.argv[1]
for name in ("report.json", "report.txt", "manifest.json"):
    path = os.path.join(run_dir, name)
    assert os.path.isfile(path) and not os.path.islink(path) and os.path.getsize(path) > 0, path
with open(os.path.join(run_dir, "report.json"), encoding="utf-8") as stream:
    report = json.load(stream)
assert report["summary"]["result"] == "failed"
for cell_id in (
    "001_health_c1_r1",
    "002_auth_me_c1_r1",
    "003_files_list_c1_r1",
    "004_music_list_c1_r1",
    "005_upload_1kb_c1_r1",
):
    row = next(row for row in report["results"] if row["raw_file"] == f"raw/{cell_id}.txt")
    assert row["result"] == "failed" and row["error"] == "invalid_rps"
assert all(math.isfinite(row["rps"]) for row in report["results"])
assert all(row["rps"] > 0 for row in report["results"] if row["result"] in {"passed", "overloaded"})
with open(os.path.join(run_dir, "manifest.json"), encoding="utf-8") as stream:
    manifest = json.load(stream)
assert manifest["state"] == "failed" and manifest["exit_code"] == 1
PY
  then
    fail '非法 RPS 必须保留终态失败报告且不写入非有限数'
    failures=$((failures + 1))
  fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_invalid_completed_rps_is_rerun_on_resume() {
  local failures=0
  local root run_id manifest invalid_value profile overloaded_cell
  local -a resume_invalid_values=()

  for profile in full overload; do
    setup_fixture "rps-resume-invalid-rps-$profile"
    root="$CURRENT_FIXTURE"
    case "$profile" in
      full)
        run_id=20260803_121011
        resume_invalid_values=(0 NaN)
        ;;
      overload)
        run_id=20260803_121021
        overloaded_cell=001_health_c1_r1
        resume_invalid_values=(-1 Infinity)
        ;;
    esac
    configure_rps_environment 1 fixture-rps-resume-invalid
    if [ "$profile" = overload ]; then BENCHMARK_ENV+=("FAKE_WRK_OVERLOAD_CELL=$overloaded_cell"); fi
    run_benchmark "$root" rps "$profile" --run-id "$run_id"
    BENCHMARK_ENV=()
    if ! assert_benchmark_status 0 "$profile 初始 RPS 运行必须成功"; then failures=$((failures + 1)); fi
    manifest="$root/benchmark/report/rps/$profile/$run_id/manifest.json"
    for invalid_value in "${resume_invalid_values[@]}"; do
      if ! /usr/bin/python3 - "$manifest" "$invalid_value" <<'PY'
import json
import sys

manifest_file, invalid_value = sys.argv[1:]
values = {
    "0": 0.0,
    "-1": -1.0,
    "NaN": float("nan"),
    "Infinity": float("inf"),
}
with open(manifest_file, encoding="utf-8") as stream:
    manifest = json.load(stream)
cell = manifest["cells"]["001_health_c1_r1"]
assert cell["state"] in {"passed", "overloaded"}
cell["record"]["rps"] = values[invalid_value]
with open(manifest_file, "w", encoding="utf-8") as stream:
    json.dump(manifest, stream)
PY
      then
        fail "$profile 注入非法 RPS 续跑夹具失败"
        failures=$((failures + 1))
      fi
      : > "$root/wrk.log"
      configure_rps_environment 1 fixture-rps-resume-invalid
      if [ "$profile" = overload ]; then BENCHMARK_ENV+=("FAKE_WRK_OVERLOAD_CELL=$overloaded_cell"); fi
      run_benchmark "$root" rps "$profile" --resume "$run_id"
      BENCHMARK_ENV=()
      if ! assert_benchmark_status 0 "$profile 非法已完成 RPS 必须允许续跑"; then failures=$((failures + 1)); fi
      if ! awk '$0 == "001_health_c1_r1" { found = 1 } END { exit(found ? 0 : 1) }' "$root/wrk.log"; then
        fail "$profile RPS=$invalid_value 的已完成单元必须重新执行"
        failures=$((failures + 1))
      fi
      if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
    done
  done
  [ "$failures" -eq 0 ]
}

test_rps_startup_signal_writes_profile_reports() {
  local failures=0
  local root pid rc=0 attempt signal expected_rc

  for signal in TERM INT; do
    case "$signal" in
      TERM) expected_rc=143 ;;
      INT) expected_rc=130 ;;
    esac
    setup_fixture "rps-startup-signal-$signal"
    root="$CURRENT_FIXTURE"
    env --default-signal=INT \
    "PATH=$root/fake-bin:$PATH" \
    "FAKE_WRK_LOG=$root/wrk.log" \
    "FAKE_WRK_URL_LOG=$root/wrk-url.log" \
    "FAKE_TOOL_LOG=$root/tools.log" \
    "FAKE_DOCKER_LOG=$root/docker.log" \
    "FAKE_LEGACY_DIR=$root/benchmark/reports" \
    "FAKE_LEGACY_ACCESS_LOG=$root/legacy-access.log" \
    "FAKE_DATE_STATE=$root/date.state" \
    "FAKE_STATE_DIR=$root/state" \
    "FAKE_DOCKER_REPORT_ROOT=$root/benchmark/report/rps" \
    FAKE_DOCKER_REPORT_RUN_ID=20260803_120003 \
    FAKE_DOCKER_REPORT_PROFILES=full \
    FAKE_DOCKER_RUNTIME_FINGERPRINT_SLEEP_SECONDS=2 \
    RPS_BASE_URL= \
    bash "$root/scripts/benchmark.sh" rps full --run-id 20260803_120003 \
    > "$root/startup-signal.log" 2>&1 &
    pid=$!
    for attempt in $(seq 1 60); do
      if awk '$1 == "runtime-fingerprint" { found = 1 } END { exit(found ? 0 : 1) }' "$root/docker.log"; then break; fi
      /usr/bin/sleep 0.05
    done
    if ! awk '$1 == "runtime-fingerprint" { found = 1 } END { exit(found ? 0 : 1) }' "$root/docker.log"; then
      fail "$signal 启动信号夹具未进入 runtime-fingerprint"
      kill "$pid" 2>/dev/null || true
    else
      kill "-$signal" "$pid"
    fi
    set +e
    wait "$pid"
    rc=$?
    set -e
    if ! assert_equals "$expected_rc" "$rc" "$signal 启动阶段必须保留信号退出码"; then failures=$((failures + 1)); fi
    if ! assert_rps_failed_profile_artifacts "$root" full 20260803_120003 "$expected_rc" \
      "$signal runtime-fingerprint 阶段"; then failures=$((failures + 1)); fi
    if ! assert_docker_operation_count "$root" down 1 "$signal 启动阶段必须恰好 down 一次"; then failures=$((failures + 1)); fi
  done
  [ "$failures" -eq 0 ]
}

test_rps_signal_after_prepare_writes_profile_reports() {
  local failures=0
  local root pid rc=0 attempt signal expected_rc run_id=20260803_120004 run_dir

  for signal in TERM INT; do
    case "$signal" in
      TERM) expected_rc=143 ;;
      INT) expected_rc=130 ;;
    esac
    setup_fixture "rps-prepare-signal-$signal"
    root="$CURRENT_FIXTURE"
    run_dir="$root/benchmark/report/rps/full/$run_id"
    env --default-signal=INT \
    "PATH=$root/fake-bin:$PATH" \
    "FAKE_WRK_LOG=$root/wrk.log" \
    "FAKE_WRK_URL_LOG=$root/wrk-url.log" \
    "FAKE_TOOL_LOG=$root/tools.log" \
    "FAKE_DOCKER_LOG=$root/docker.log" \
    "FAKE_LEGACY_DIR=$root/benchmark/reports" \
    "FAKE_LEGACY_ACCESS_LOG=$root/legacy-access.log" \
    "FAKE_DATE_STATE=$root/date.state" \
    "FAKE_STATE_DIR=$root/state" \
    "FAKE_DOCKER_REPORT_ROOT=$root/benchmark/report/rps" \
    "FAKE_DOCKER_REPORT_RUN_ID=$run_id" \
    FAKE_DOCKER_REPORT_PROFILES=full \
    RPS_BASE_URL= \
    RPS_READ_CONCURRENCY=1 \
    RPS_TRANSFER_CONCURRENCY=1 \
    RPS_UPLOAD_CONCURRENCY=1 \
    RPS_REPEATS=1 \
    RPS_DURATION_SECONDS=1 \
    RPS_UPLOAD_DURATION_SECONDS=1 \
    RPS_UPLOAD_DELAY_MILLISECONDS=0 \
    RPS_CELL_TIMEOUT_GRACE_SECONDS=5 \
    RPS_REQUEST_TIMEOUT_SECONDS=1 \
    RPS_TEST_PAUSE_AFTER_PREPARE_SECONDS=1 \
    bash "$root/scripts/benchmark.sh" rps full --run-id "$run_id" \
    > "$root/prepare-signal.log" 2>&1 &
    pid=$!
    for attempt in $(seq 1 60); do
      [ -d "$run_dir" ] && [ ! -e "$run_dir/manifest.json" ] && break
      /usr/bin/sleep 0.05
    done
    if [ ! -d "$run_dir" ] || [ -e "$run_dir/manifest.json" ]; then
      fail "$signal prepare 信号夹具未停在创建运行目录后"
      kill "$pid" 2>/dev/null || true
    else
      kill "-$signal" "$pid"
    fi
    set +e
    wait "$pid"
    rc=$?
    set -e
    if ! assert_equals "$expected_rc" "$rc" "$signal prepare 阶段必须保留信号退出码"; then failures=$((failures + 1)); fi
    if ! assert_rps_failed_profile_artifacts "$root" full "$run_id" "$expected_rc" \
      "$signal 创建运行目录后"; then failures=$((failures + 1)); fi
    if ! assert_docker_operation_count "$root" down 1 "$signal prepare 阶段必须恰好 down 一次"; then failures=$((failures + 1)); fi
  done
  [ "$failures" -eq 0 ]
}

test_rps_overload_resume_is_complete() {
  local failures=0
  local root="$CURRENT_FIXTURE"
  local run_id=20260802_123010
  local manifest="$root/benchmark/report/rps/overload/$run_id/manifest.json"

  setup_fixture rps-overload-resume
  root="$CURRENT_FIXTURE"
  manifest="$root/benchmark/report/rps/overload/$run_id/manifest.json"
  configure_rps_environment 1 fixture-rps-overload
  BENCHMARK_ENV+=(FAKE_WRK_OVERLOAD_CELL=001_health_c1_r1)
  run_benchmark "$root" rps overload --run-id "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 'overload 单元出现观测错误仍应作为完成'; then failures=$((failures + 1)); fi
  if ! /usr/bin/python3 - "$manifest" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    manifest = json.load(stream)
assert manifest["cells"]["001_health_c1_r1"]["state"] == "overloaded"
PY
  then
    fail 'overload manifest 必须记录 overloaded 单元状态'
    failures=$((failures + 1))
  fi
  : > "$root/wrk.log"
  configure_rps_environment 1 fixture-rps-overload
  run_benchmark "$root" rps overload --resume "$run_id"
  BENCHMARK_ENV=()
  if ! assert_benchmark_status 0 'overload 的 overloaded 单元 resume 应完成'; then failures=$((failures + 1)); fi
  if ! assert_empty_file "$root/wrk.log" 'overloaded 单元在同指纹 resume 时不得重跑'; then failures=$((failures + 1)); fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_rps_signal_cleans_managed_environment() {
  local failures=0
  local root pid rc=0 attempt signal expected_rc

  for signal in TERM INT; do
    case "$signal" in
      TERM) expected_rc=143 ;;
      INT) expected_rc=130 ;;
    esac
    setup_fixture "rps-managed-signal-$signal"
    root="$CURRENT_FIXTURE"
    env --default-signal=INT \
    "PATH=$root/fake-bin:$PATH" \
    "FAKE_WRK_LOG=$root/wrk.log" \
    "FAKE_WRK_URL_LOG=$root/wrk-url.log" \
    "FAKE_TOOL_LOG=$root/tools.log" \
    "FAKE_DOCKER_LOG=$root/docker.log" \
    "FAKE_LEGACY_DIR=$root/benchmark/reports" \
    "FAKE_LEGACY_ACCESS_LOG=$root/legacy-access.log" \
    "FAKE_DATE_STATE=$root/date.state" \
    "FAKE_STATE_DIR=$root/state" \
    "FAKE_DOCKER_REPORT_ROOT=$root/benchmark/report/rps" \
    FAKE_DOCKER_REPORT_RUN_ID=20260802_123020 \
    FAKE_DOCKER_REPORT_PROFILES=full \
    RPS_BASE_URL= \
    RPS_READ_CONCURRENCY=1 \
    RPS_TRANSFER_CONCURRENCY=1 \
    RPS_UPLOAD_CONCURRENCY=1 \
    RPS_REPEATS=1 \
    RPS_DURATION_SECONDS=1 \
    RPS_UPLOAD_DURATION_SECONDS=1 \
    RPS_UPLOAD_DELAY_MILLISECONDS=0 \
    RPS_CELL_TIMEOUT_GRACE_SECONDS=5 \
    RPS_REQUEST_TIMEOUT_SECONDS=1 \
    FAKE_WRK_SLEEP_SECONDS=2 \
    bash "$root/scripts/benchmark.sh" rps full --run-id 20260802_123020 \
    > "$root/signal.log" 2>&1 &
    pid=$!
    for attempt in $(seq 1 60); do
      [ -s "$root/wrk.log" ] && break
      /usr/bin/sleep 0.05
    done
    if [ ! -s "$root/wrk.log" ]; then
      fail "$signal signal 回归夹具未进入 RPS 单元"
      kill "$pid" 2>/dev/null || true
    else
      kill "-$signal" "$pid"
    fi
    set +e
    wait "$pid"
    rc=$?
    set -e
    if ! assert_equals "$expected_rc" "$rc" "$signal 必须保留信号退出码"; then failures=$((failures + 1)); fi
    if ! assert_docker_operation_count "$root" down 1 "$signal 后托管环境必须恰好 down 一次"; then failures=$((failures + 1)); fi
    if ! assert_rps_failed_profile_artifacts "$root" full 20260802_123020 "$expected_rc" \
      "$signal 活跃 RPS profile"; then failures=$((failures + 1)); fi
  done
  [ "$failures" -eq 0 ]
}

test_rps_signal_writes_external_and_unstarted_profile_artifacts() {
  local failures=0
  local root pid rc=0 attempt signal expected_rc

  for signal in TERM INT; do
    case "$signal" in
      TERM) expected_rc=143 ;;
      INT) expected_rc=130 ;;
    esac
    setup_fixture "rps-external-signal-$signal"
    root="$CURRENT_FIXTURE"
    env --default-signal=INT \
    "PATH=$root/fake-bin:$PATH" \
    "FAKE_WRK_LOG=$root/wrk.log" \
    "FAKE_WRK_URL_LOG=$root/wrk-url.log" \
    "FAKE_TOOL_LOG=$root/tools.log" \
    "FAKE_DOCKER_LOG=$root/docker.log" \
    "FAKE_LEGACY_DIR=$root/benchmark/reports" \
    "FAKE_LEGACY_ACCESS_LOG=$root/legacy-access.log" \
    "FAKE_DATE_STATE=$root/date.state" \
    "FAKE_STATE_DIR=$root/state" \
    RPS_BASE_URL=http://external.fake \
    RPS_TARGET_FINGERPRINT=external-signal-v1 \
    RPS_READ_CONCURRENCY=1 \
    RPS_TRANSFER_CONCURRENCY=1 \
    RPS_UPLOAD_CONCURRENCY=1 \
    RPS_REPEATS=1 \
    RPS_DURATION_SECONDS=1 \
    RPS_UPLOAD_DURATION_SECONDS=1 \
    RPS_UPLOAD_DELAY_MILLISECONDS=0 \
    RPS_CELL_TIMEOUT_GRACE_SECONDS=5 \
    RPS_REQUEST_TIMEOUT_SECONDS=1 \
    FAKE_WRK_SLEEP_SECONDS=2 \
    bash "$root/scripts/benchmark.sh" rps full --run-id 20260803_122001 \
    > "$root/external-signal.log" 2>&1 &
    pid=$!
    for attempt in $(seq 1 60); do
      [ -s "$root/wrk.log" ] && break
      /usr/bin/sleep 0.05
    done
    if [ ! -s "$root/wrk.log" ]; then
      fail "$signal 外部 URL 信号夹具未进入 RPS 单元"
      kill "$pid" 2>/dev/null || true
    else
      kill "-$signal" "$pid"
    fi
    set +e
    wait "$pid"
    rc=$?
    set -e
    if ! assert_equals "$expected_rc" "$rc" "$signal 外部 URL RPS 必须保留信号退出码"; then failures=$((failures + 1)); fi
    if ! assert_rps_failed_profile_artifacts "$root" full 20260803_122001 "$expected_rc" \
      "$signal 外部 URL 活跃 RPS profile"; then failures=$((failures + 1)); fi
    if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  done

  setup_fixture rps-managed-multiprofile-signal
  root="$CURRENT_FIXTURE"
  env --default-signal=INT \
  "PATH=$root/fake-bin:$PATH" \
  "FAKE_WRK_LOG=$root/wrk.log" \
  "FAKE_WRK_URL_LOG=$root/wrk-url.log" \
  "FAKE_TOOL_LOG=$root/tools.log" \
  "FAKE_DOCKER_LOG=$root/docker.log" \
  "FAKE_LEGACY_DIR=$root/benchmark/reports" \
  "FAKE_LEGACY_ACCESS_LOG=$root/legacy-access.log" \
  "FAKE_DATE_STATE=$root/date.state" \
  "FAKE_STATE_DIR=$root/state" \
  "FAKE_DOCKER_REPORT_ROOT=$root/benchmark/report/rps" \
  FAKE_DOCKER_REPORT_RUN_ID=20260803_122002 \
  'FAKE_DOCKER_REPORT_PROFILES=full overload' \
  RPS_BASE_URL= \
  RPS_READ_CONCURRENCY=1 \
  RPS_TRANSFER_CONCURRENCY=1 \
  RPS_UPLOAD_CONCURRENCY=1 \
  RPS_REPEATS=1 \
  RPS_DURATION_SECONDS=1 \
  RPS_UPLOAD_DURATION_SECONDS=1 \
  RPS_UPLOAD_DELAY_MILLISECONDS=0 \
  RPS_CELL_TIMEOUT_GRACE_SECONDS=5 \
  RPS_REQUEST_TIMEOUT_SECONDS=1 \
  FAKE_WRK_SLEEP_SECONDS=2 \
  bash "$root/scripts/benchmark.sh" rps full overload --run-id 20260803_122002 \
  > "$root/multiprofile-signal.log" 2>&1 &
  pid=$!
  for attempt in $(seq 1 60); do
    [ -s "$root/wrk.log" ] && break
    /usr/bin/sleep 0.05
  done
  if [ ! -s "$root/wrk.log" ]; then
    fail '多 profile 信号夹具未进入 full RPS 单元'
    kill "$pid" 2>/dev/null || true
  else
    kill -TERM "$pid"
  fi
  set +e
  wait "$pid"
  rc=$?
  set -e
  if ! assert_equals 143 "$rc" '多 profile RPS 必须保留 TERM 退出码'; then failures=$((failures + 1)); fi
  for profile in full overload; do
    if ! assert_rps_failed_profile_artifacts "$root" "$profile" 20260803_122002 143 \
      "多 profile TERM 的 $profile"; then failures=$((failures + 1)); fi
  done
  if ! assert_docker_operation_count "$root" down 1 '多 profile TERM 后托管环境必须恰好 down 一次'; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_diff_uses_new_manifests() {
  local failures=0
  setup_fixture diff-new-structure
  local root="$CURRENT_FIXTURE"
  local micro_legacy_marker=LEGACY_MICRO_DIFF_POISON
  local qps_legacy_marker=LEGACY_QPS_DIFF_POISON
  local rps_legacy_marker=LEGACY_RPS_DIFF_POISON
  local micro_aggregate_marker=DIFF_MICRO_AGGREGATE_POISON
  local qps_aggregate_marker=DIFF_QPS_AGGREGATE_POISON
  local rps_aggregate_marker=DIFF_RPS_AGGREGATE_POISON
  write_target "$root" bench_alpha
  write_target "$root" qps_alpha
  write_qps_source "$root" qps_alpha
  write_diff_manifest_series "$root" micro bench_alpha default DIFF_MICRO
  write_diff_manifest_series "$root" qps qps_alpha full DIFF_QPS
  write_diff_manifest_series "$root" rps full full DIFF_RPS
  write_misplaced_manifest "$root" micro bench_alpha default 20260802_000019 bench_wrong 20260802_000019 \
    DIFF_MICRO_WRONG_TARGET
  write_misplaced_manifest "$root" qps qps_alpha full 20260802_000020 qps_wrong 20260802_000020 \
    DIFF_QPS_WRONG_TARGET
  write_misplaced_manifest "$root" rps full full 20260802_000021 wrong_profile 20260802_000021 \
    DIFF_RPS_WRONG_PROFILE
  write_misplaced_manifest "$root" micro bench_alpha default 20260802_000023 bench_alpha 20260802_000022 \
    DIFF_MICRO_WRONG_RUN_ID
  write_legacy_report "$root" micro 20990101_000001 "$micro_legacy_marker"
  write_legacy_report "$root" qps 20990101_000001 "$qps_legacy_marker"
  write_legacy_report "$root" rps 20990101_000001 "$rps_legacy_marker"
  local legacy_before
  legacy_before="$(snapshot_tree "$root/benchmark/reports")"

  run_benchmark "$root" diff micro bench_alpha
  local micro_diff_log="$BENCHMARK_LOG"
  if ! assert_benchmark_status 0 'diff micro bench_alpha 新结构接口'; then failures=$((failures + 1)); fi
  if ! assert_diff_selects_latest_complete_pair "$micro_diff_log" 'diff micro bench_alpha' "$micro_legacy_marker" \
    "$micro_aggregate_marker"; then
    failures=$((failures + 1))
  fi

  run_benchmark "$root" diff qps qps_alpha full
  local qps_diff_log="$BENCHMARK_LOG"
  if ! assert_benchmark_status 0 'diff qps qps_alpha full 新结构接口'; then failures=$((failures + 1)); fi
  if ! assert_diff_selects_latest_complete_pair "$qps_diff_log" 'diff qps qps_alpha full' "$qps_legacy_marker" \
    "$qps_aggregate_marker"; then
    failures=$((failures + 1))
  fi

  run_benchmark "$root" diff rps full
  local rps_diff_log="$BENCHMARK_LOG"
  if ! assert_benchmark_status 0 'diff rps full 新结构接口'; then failures=$((failures + 1)); fi
  if ! assert_diff_selects_latest_complete_pair "$rps_diff_log" 'diff rps full' "$rps_legacy_marker" \
    "$rps_aggregate_marker"; then
    failures=$((failures + 1))
  fi

  if ! assert_legacy_not_written "$root" "$legacy_before"; then failures=$((failures + 1)); fi
  report_legacy_access "$root"
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

test_diff_without_reports_is_read_only() {
  local failures=0
  setup_fixture diff-read-only
  local root="$CURRENT_FIXTURE"

  run_benchmark "$root" diff micro bench_alpha
  if ! assert_benchmark_status 1 '无报告时 diff micro'; then failures=$((failures + 1)); fi
  if [ -e "$root/benchmark/report" ]; then
    fail '纯读 diff 在报告根不存在时不得创建目录'
    failures=$((failures + 1))
  fi
  if ! assert_no_docker "$root"; then failures=$((failures + 1)); fi
  [ "$failures" -eq 0 ]
}

run_case() {
  local name="$1"
  shift
  printf '\n=== %s ===\n' "$name"
  if "$@"; then
    printf '通过: %s\n' "$name"
    return 0
  fi
  printf '失败: %s\n' "$name" >&2
  return 1
}

failed_cases=0
case "${BENCHMARK_TEST_GROUP:-all}" in
  all)
    run_case '运行 ID、原子目标清单与损坏续跑' test_run_id_and_manifest_contract || failed_cases=$((failed_cases + 1))
    run_case '报告路径不污染 Git 指纹且业务变化仍失效' test_generated_reports_preserve_git_fingerprint || failed_cases=$((failed_cases + 1))
    run_case '正常 BENCH_FLAGS 摘要化且敏感值执行前拒绝' \
      test_bench_flags_are_hashed_and_sensitive_values_are_rejected || failed_cases=$((failed_cases + 1))
    run_case '布尔整数 manifest 被 diff 与 resume 拒绝' test_boolean_manifest_integers_are_rejected || failed_cases=$((failed_cases + 1))
    run_case '毒化 artifact 被 diff 与 resume 拒绝' test_manifest_artifact_poisoning_is_rejected || failed_cases=$((failed_cases + 1))
    run_case '非对象 JSON 被安静地 diff 与 resume 拒绝' test_non_object_manifests_are_quietly_rejected || failed_cases=$((failed_cases + 1))
    run_case '非 UTF-8 manifest 被安静地拒绝' test_non_utf8_manifests_are_quietly_rejected || failed_cases=$((failed_cases + 1))
    run_case 'resume 目录链接被拒绝且不触碰目录外内容' test_resume_directory_symlinks_are_rejected || failed_cases=$((failed_cases + 1))
    run_case 'resume artifact 链接被安全重建' test_resume_artifact_links_are_replaced_safely || failed_cases=$((failed_cases + 1))
    run_case '错误物理目录和 JSON 身份清单不进入 diff 或 resume' \
      test_resume_manifest_identity_is_bound_to_current_location || failed_cases=$((failed_cases + 1))
    run_case '运行目录替换时 artifact 不发布到目录外' \
      test_run_directory_replacement_does_not_publish_artifacts_outside || failed_cases=$((failed_cases + 1))
    run_case '目标级微基准/QPS 分层 manifest 与旧目录隔离' test_target_level_layout || failed_cases=$((failed_cases + 1))
    run_case '相同指纹 --resume 仅补跑失败目标' test_resume_failed_target_only || failed_cases=$((failed_cases + 1))
    run_case '二进制、BENCH_FLAGS 与 QPS_PROFILE 指纹变化重跑通过目标' \
      test_fingerprint_changes_rerun_passed_targets || failed_cases=$((failed_cases + 1))
    run_case 'QPS failed、timeout、running、损坏和 passed 续跑' \
      test_qps_resume_all_incomplete_states || failed_cases=$((failed_cases + 1))
    run_case 'RPS 单元续跑与 RPS_TARGET_FINGERPRINT 变化' test_rps_resume_and_fingerprint || failed_cases=$((failed_cases + 1))
    run_case 'RPS resume 报告重建完整矩阵' test_rps_resume_reports_rebuild_complete_matrix || failed_cases=$((failed_cases + 1))
    run_case 'RPS 损坏 cell 字段必须补跑' test_rps_corrupt_cells_are_rerun || failed_cases=$((failed_cases + 1))
    run_case 'RPS resume 清除额外 manifest cell' test_rps_extra_manifest_cells_are_pruned_on_resume || failed_cases=$((failed_cases + 1))
    run_case 'RPS resume artifact 链接安全重建' test_rps_resume_artifacts_replace_links_without_following_them || failed_cases=$((failed_cases + 1))
    run_case 'RPS timeout 报告失败一致性' test_rps_timeout_is_reported_as_failed || failed_cases=$((failed_cases + 1))
    run_case 'RPS 多 profile、托管环境与清理生命周期' test_rps_profiles_and_managed_lifecycle || failed_cases=$((failed_cases + 1))
    run_case 'RPS 托管启动失败保留各 profile 报告' \
      test_rps_managed_startup_failures_write_profile_reports || failed_cases=$((failed_cases + 1))
    run_case 'RPS 非有限指标必须生成失败报告' test_rps_invalid_rps_metrics_are_failed || failed_cases=$((failed_cases + 1))
    run_case 'RPS 非法已完成吞吐量必须续跑' test_rps_invalid_completed_rps_is_rerun_on_resume || failed_cases=$((failed_cases + 1))
    run_case 'RPS overload completed 续跑语义' test_rps_overload_resume_is_complete || failed_cases=$((failed_cases + 1))
    run_case 'RPS INT/TERM 托管环境清理' test_rps_signal_cleans_managed_environment || failed_cases=$((failed_cases + 1))
    run_case 'RPS 外部及多 profile 信号终态报告' test_rps_signal_writes_external_and_unstarted_profile_artifacts || failed_cases=$((failed_cases + 1))
    run_case 'RPS 启动阶段 INT/TERM 保留报告' test_rps_startup_signal_writes_profile_reports || failed_cases=$((failed_cases + 1))
    run_case 'RPS 创建运行目录后 INT/TERM 保留报告' test_rps_signal_after_prepare_writes_profile_reports || failed_cases=$((failed_cases + 1))
    run_case '新结构 diff 只选择最近两份 complete manifest' test_diff_uses_new_manifests || failed_cases=$((failed_cases + 1))
    run_case '无报告 diff 不创建报告根目录' test_diff_without_reports_is_read_only || failed_cases=$((failed_cases + 1))
    ;;
  micro_qps)
    run_case '运行 ID、原子目标清单与损坏续跑' test_run_id_and_manifest_contract || failed_cases=$((failed_cases + 1))
    run_case '报告路径不污染 Git 指纹且业务变化仍失效' test_generated_reports_preserve_git_fingerprint || failed_cases=$((failed_cases + 1))
    run_case '正常 BENCH_FLAGS 摘要化且敏感值执行前拒绝' \
      test_bench_flags_are_hashed_and_sensitive_values_are_rejected || failed_cases=$((failed_cases + 1))
    run_case '布尔整数 manifest 被 diff 与 resume 拒绝' test_boolean_manifest_integers_are_rejected || failed_cases=$((failed_cases + 1))
    run_case '毒化 artifact 被 diff 与 resume 拒绝' test_manifest_artifact_poisoning_is_rejected || failed_cases=$((failed_cases + 1))
    run_case '非对象 JSON 被安静地 diff 与 resume 拒绝' test_non_object_manifests_are_quietly_rejected || failed_cases=$((failed_cases + 1))
    run_case '非 UTF-8 manifest 被安静地拒绝' test_non_utf8_manifests_are_quietly_rejected || failed_cases=$((failed_cases + 1))
    run_case 'resume 目录链接被拒绝且不触碰目录外内容' test_resume_directory_symlinks_are_rejected || failed_cases=$((failed_cases + 1))
    run_case 'resume artifact 链接被安全重建' test_resume_artifact_links_are_replaced_safely || failed_cases=$((failed_cases + 1))
    run_case '错误物理目录和 JSON 身份清单不进入 diff 或 resume' \
      test_resume_manifest_identity_is_bound_to_current_location || failed_cases=$((failed_cases + 1))
    run_case '运行目录替换时 artifact 不发布到目录外' \
      test_run_directory_replacement_does_not_publish_artifacts_outside || failed_cases=$((failed_cases + 1))
    run_case '目标级微基准/QPS 分层 manifest 与旧目录隔离' test_target_level_layout || failed_cases=$((failed_cases + 1))
    run_case '相同指纹 --resume 仅补跑失败目标' test_resume_failed_target_only || failed_cases=$((failed_cases + 1))
    run_case '二进制、BENCH_FLAGS 与 QPS_PROFILE 指纹变化重跑通过目标' \
      test_fingerprint_changes_rerun_passed_targets || failed_cases=$((failed_cases + 1))
    run_case 'QPS failed、timeout、running、损坏和 passed 续跑' \
      test_qps_resume_all_incomplete_states || failed_cases=$((failed_cases + 1))
    run_case '新结构 diff 只选择最近两份 complete manifest' test_diff_uses_new_manifests || failed_cases=$((failed_cases + 1))
    run_case '无报告 diff 不创建报告根目录' test_diff_without_reports_is_read_only || failed_cases=$((failed_cases + 1))
    ;;
  rps)
    run_case 'RPS 单元续跑与 RPS_TARGET_FINGERPRINT 变化' test_rps_resume_and_fingerprint || failed_cases=$((failed_cases + 1))
    run_case 'RPS resume 报告重建完整矩阵' test_rps_resume_reports_rebuild_complete_matrix || failed_cases=$((failed_cases + 1))
    run_case 'RPS 损坏 cell 字段必须补跑' test_rps_corrupt_cells_are_rerun || failed_cases=$((failed_cases + 1))
    run_case 'RPS resume artifact 链接安全重建' test_rps_resume_artifacts_replace_links_without_following_them || failed_cases=$((failed_cases + 1))
    run_case 'RPS timeout 报告失败一致性' test_rps_timeout_is_reported_as_failed || failed_cases=$((failed_cases + 1))
    run_case 'RPS 多 profile、托管环境与清理生命周期' test_rps_profiles_and_managed_lifecycle || failed_cases=$((failed_cases + 1))
    run_case 'RPS 托管启动失败保留各 profile 报告' \
      test_rps_managed_startup_failures_write_profile_reports || failed_cases=$((failed_cases + 1))
    run_case 'RPS 非有限指标必须生成失败报告' test_rps_invalid_rps_metrics_are_failed || failed_cases=$((failed_cases + 1))
    run_case 'RPS overload completed 续跑语义' test_rps_overload_resume_is_complete || failed_cases=$((failed_cases + 1))
    run_case 'RPS INT/TERM 托管环境清理' test_rps_signal_cleans_managed_environment || failed_cases=$((failed_cases + 1))
    run_case 'RPS 启动阶段 INT/TERM 保留报告' test_rps_startup_signal_writes_profile_reports || failed_cases=$((failed_cases + 1))
    ;;
  rps_resume)
    run_case 'RPS 单元续跑与 RPS_TARGET_FINGERPRINT 变化' test_rps_resume_and_fingerprint || failed_cases=$((failed_cases + 1))
    ;;
  rps_rebuild)
    run_case 'RPS resume 报告重建完整矩阵' test_rps_resume_reports_rebuild_complete_matrix || failed_cases=$((failed_cases + 1))
    ;;
  rps_corrupt)
    run_case 'RPS 损坏 cell 字段必须补跑' test_rps_corrupt_cells_are_rerun || failed_cases=$((failed_cases + 1))
    run_case 'RPS resume 清除额外 manifest cell' test_rps_extra_manifest_cells_are_pruned_on_resume || failed_cases=$((failed_cases + 1))
    ;;
  rps_artifacts)
    run_case 'RPS resume artifact 链接安全重建' test_rps_resume_artifacts_replace_links_without_following_them || failed_cases=$((failed_cases + 1))
    ;;
  rps_timeout)
    run_case 'RPS timeout 报告失败一致性' test_rps_timeout_is_reported_as_failed || failed_cases=$((failed_cases + 1))
    ;;
  rps_lifecycle)
    run_case 'RPS 多 profile、托管环境与清理生命周期' test_rps_profiles_and_managed_lifecycle || failed_cases=$((failed_cases + 1))
    ;;
  rps_startup)
    run_case 'RPS 托管启动失败保留各 profile 报告' \
      test_rps_managed_startup_failures_write_profile_reports || failed_cases=$((failed_cases + 1))
    ;;
  rps_overload)
    run_case 'RPS overload completed 续跑语义' test_rps_overload_resume_is_complete || failed_cases=$((failed_cases + 1))
    ;;
  rps_signal)
    run_case 'RPS INT/TERM 托管环境清理' test_rps_signal_cleans_managed_environment || failed_cases=$((failed_cases + 1))
    run_case 'RPS 外部及多 profile 信号终态报告' test_rps_signal_writes_external_and_unstarted_profile_artifacts || failed_cases=$((failed_cases + 1))
    run_case 'RPS 启动阶段 INT/TERM 保留报告' test_rps_startup_signal_writes_profile_reports || failed_cases=$((failed_cases + 1))
    run_case 'RPS 创建运行目录后 INT/TERM 保留报告' test_rps_signal_after_prepare_writes_profile_reports || failed_cases=$((failed_cases + 1))
    ;;
  rps_prepare_signal)
    run_case 'RPS 创建运行目录后 INT/TERM 保留报告' test_rps_signal_after_prepare_writes_profile_reports || failed_cases=$((failed_cases + 1))
    ;;
  rps_integrity)
    run_case 'RPS 非有限指标必须生成失败报告' test_rps_invalid_rps_metrics_are_failed || failed_cases=$((failed_cases + 1))
    run_case 'RPS 非法已完成吞吐量必须续跑' test_rps_invalid_completed_rps_is_rerun_on_resume || failed_cases=$((failed_cases + 1))
    ;;
  *)
    printf '错误: BENCHMARK_TEST_GROUP 仅支持 all、micro_qps、rps 或 RPS 子组\n' >&2
    exit 2
    ;;
esac

if [ "$failed_cases" -ne 0 ]; then
  printf '基准报告重构回归测试处于 RED：%s 个场景失败\n' "$failed_cases" >&2
  exit 1
fi
printf '基准报告重构回归测试通过\n'
