#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"
source "$(cd "$(dirname "$0")" && pwd)/lib/isolated_docker_env.sh"
cd "$PROJECT_ROOT"

run_frontend_tests() {
  yellow "=== 前端 Vitest ==="
  if [ ! -d "$FRONTEND_DIR" ]; then
    yellow "未发现 frontend 目录，跳过前端 Vitest"
    return 0
  fi

  ensure_frontend_dependencies
  (
    cd "$FRONTEND_DIR"
    npm run test -- "$@"
  )
  green "前端 Vitest 通过"
}

run_script_regression_tests() {
  yellow "=== 脚本回归测试 ==="
  bash "$PROJECT_ROOT/tests/test_codeql_discovery.sh"
  bash "$PROJECT_ROOT/tests/test_lint_script.sh"
  bash "$PROJECT_ROOT/tests/test_docker_admin_env.sh"
  bash "$PROJECT_ROOT/tests/test_test_script.sh"
  bash "$PROJECT_ROOT/tests/test_benchmark_script.sh"
  bash "$PROJECT_ROOT/tests/test_bench_http_server.sh"
  bash "$PROJECT_ROOT/tests/test_migrate_legacy_reports.sh"
  green "脚本回归测试通过"
}

validate_frontend_test_path() {
  local path="$1"
  [[ "$path" == tests/* ]] || return 1
  [[ "$path" != /* && "$path" != -* && "$path" != *".."* ]] || return 1
  [[ "$path" =~ \.(test|spec)\.(ts|tsx)$ ]]
}

validate_e2e_test_path() {
  local path="$1"
  [[ "$path" == tests/e2e/* ]] || return 1
  [[ "$path" != /* && "$path" != -* && "$path" != *".."* ]] || return 1
  [[ "$path" =~ \.spec\.ts$ ]]
}

run_e2e_tests() {
  local playwright_args=("$@")
  local timestamp random_suffix run_id e2e_base_url
  local playwright_pid="" playwright_rc
  local admin_username admin_email admin_password normal_one_username normal_one_email normal_one_password
  local normal_two_username normal_two_email normal_two_password
  local cleanup_rc=0 e2e_cleanup_done=0 start_rc=0
  timestamp="$(date '+%Y%m%d_%H%M%S')"
  random_suffix="$(openssl rand -hex 4)"
  run_id="${timestamp}_$$_${random_suffix}"

  cleanup_e2e() {
    local helper_cleanup_rc=0
    if [ "$e2e_cleanup_done" -ne 0 ]; then
      return 0
    fi
    e2e_cleanup_done=1
    trap - EXIT
    trap '' INT TERM
    hps_cleanup_isolated_environment || helper_cleanup_rc=$?
    return "$helper_cleanup_rc"
  }
  read_isolated_env_value() {
    local key="$1"
    awk -v key="$key" '
      index($0, key "=") == 1 {
        print substr($0, length(key) + 2)
        found = 1
        exit
      }
      END { exit(found ? 0 : 1) }
    ' "$HPS_ISOLATED_ENV_FILE"
  }
  forward_e2e_signal() {
    local signal="$1" signal_rc="$2"
    trap '' INT TERM
    if [ -n "$playwright_pid" ]; then
      kill -s "$signal" -- "-$playwright_pid" 2>/dev/null || true
      local grace_deadline=$((SECONDS + 2))
      # 正常响应时立即继续；仅对忽略首个信号的进程组施加有界宽限。
      while kill -0 -- "-$playwright_pid" 2>/dev/null; do
        if [ "$SECONDS" -ge "$grace_deadline" ]; then
          kill -KILL -- "-$playwright_pid" 2>/dev/null || true
          break
        fi
        sleep 0.1
      done
      wait "$playwright_pid" 2>/dev/null || true
    fi
    cleanup_e2e || true
    exit "$signal_rc"
  }

  HPS_ISOLATED_PROJECT_NAME="hps_e2e_${run_id}"
  HPS_ISOLATED_ENV_FILE=""
  HPS_ISOLATED_BASE_URL=""
  HPS_ISOLATED_TEMP_DIR=""
  trap 'original_rc=$?; cleanup_rc=0; cleanup_e2e || cleanup_rc=$?; if [ "$original_rc" -ne 0 ]; then exit "$original_rc"; else exit "$cleanup_rc"; fi' EXIT
  trap 'forward_e2e_signal INT 130' INT
  trap 'forward_e2e_signal TERM 143' TERM

  if hps_start_isolated_environment e2e "$run_id"; then
    :
  else
    start_rc=$?
    e2e_cleanup_done=1
    trap - EXIT
    trap - INT TERM
    return "$start_rc"
  fi

  if admin_username="$(read_isolated_env_value ADMIN_USERNAME)"; then :; else return $?; fi
  if admin_email="$(read_isolated_env_value ADMIN_EMAIL)"; then :; else return $?; fi
  if admin_password="$(read_isolated_env_value ADMIN_PASSWORD)"; then :; else return $?; fi
  normal_one_username="normal_a_${run_id}"
  normal_one_email="${normal_one_username}@example.invalid"
  if normal_one_password="$(openssl rand -hex 24)"; then
    normal_one_password="E2eNormalA_${random_suffix}_${normal_one_password}"
  else
    return $?
  fi
  normal_two_username="normal_b_${run_id}"
  normal_two_email="${normal_two_username}@example.invalid"
  if normal_two_password="$(openssl rand -hex 24)"; then
    normal_two_password="E2eNormalB_${random_suffix}_${normal_two_password}"
  else
    return $?
  fi

  yellow "=== 隔离 Playwright E2E: $run_id ==="
  e2e_base_url="$HPS_ISOLATED_BASE_URL"
  ensure_frontend_dependencies
  set +e
  (
    cd "$FRONTEND_DIR"
    PLAYWRIGHT_BASE_URL="$e2e_base_url" \
      E2E_BASE_URL="$e2e_base_url" \
      PLAYWRIGHT_RUN_ID="$run_id" \
      E2E_RUN_ID="$run_id" \
      E2E_ADMIN_USERNAME="$admin_username" \
      E2E_ADMIN_EMAIL="$admin_email" \
      E2E_ADMIN_PASSWORD="$admin_password" \
      E2E_NORMAL_ONE_USERNAME="$normal_one_username" \
      E2E_NORMAL_ONE_EMAIL="$normal_one_email" \
      E2E_NORMAL_ONE_PASSWORD="$normal_one_password" \
      E2E_NORMAL_TWO_USERNAME="$normal_two_username" \
      E2E_NORMAL_TWO_EMAIL="$normal_two_email" \
      E2E_NORMAL_TWO_PASSWORD="$normal_two_password" \
      exec setsid npm run test:e2e -- "${playwright_args[@]}"
  ) &
  playwright_pid=$!
  wait "$playwright_pid"
  playwright_rc=$?
  playwright_pid=""
  set -e
  cleanup_e2e || cleanup_rc=$?
  if [ "$playwright_rc" -ne 0 ]; then
    return "$playwright_rc"
  fi
  return "$cleanup_rc"
}


if [ "${1:-}" = "frontend" ]; then
  shift
  for test_path in "$@"; do
    if ! validate_frontend_test_path "$test_path"; then
      red "非法 frontend 测试路径: $test_path"
      exit 2
    fi
  done
  run_frontend_tests "$@"
  echo ""
  green "测试完成"
  exit 0
fi

if [ "${1:-}" = "e2e" ]; then
  shift
  e2e_args=()
  e2e_test_paths=()
  update_snapshots_seen=0
  for e2e_arg in "$@"; do
    if [ "$e2e_arg" = "--update-snapshots" ]; then
      if [ "$update_snapshots_seen" -ne 0 ]; then
        red "非法 e2e 参数: $e2e_arg"
        exit 2
      fi
      update_snapshots_seen=1
    else
      e2e_test_paths+=("$e2e_arg")
      e2e_args+=("$e2e_arg")
    fi
  done
  [ "${#e2e_test_paths[@]}" -gt 0 ] || { red "e2e 至少需要一个 spec 路径"; exit 2; }
  for test_path in "${e2e_test_paths[@]}"; do
    if ! validate_e2e_test_path "$test_path"; then
      red "非法 e2e 测试路径: $test_path"
      exit 2
    fi
  done
  if [ "$update_snapshots_seen" -ne 0 ]; then
    e2e_args+=("--update-snapshots")
  fi
  run_e2e_tests "${e2e_args[@]}"
  echo ""
  green "E2E 测试完成"
  exit 0
fi

if [ -n "${1:-}" ]; then
  test_selector="$1"
  if [[ "$test_selector" != */* ]]; then
    if [[ "$test_selector" == test_* ]]; then
      test_selector="${test_selector}/default"
    else
      red "非法测试目标: $test_selector"
      exit 2
    fi
  fi

  yellow "运行测试: $test_selector"
  xmake test "$test_selector" 2>&1
  run_frontend_tests
  run_script_regression_tests
  echo ""
  green "测试完成"
  exit 0
fi

yellow "=== 全部测试 ==="
OUT=$(mktemp)
# 用 xmake 退出码做主判断，不依赖带颜色码的文本匹配
set +e
xmake test -j1 2>&1 | tee "$OUT"
XMAKE_RC=${PIPESTATUS[0]}
set -e

if [ "$XMAKE_RC" -ne 0 ]; then
  red "存在失败的测试"
  rm -f "$OUT"
  exit 1
elif grep -qi "nothing to test\|no test" "$OUT"; then
  yellow "暂无测试用例"
else
  green "所有测试通过"
fi
rm -f "$OUT"

run_frontend_tests
run_script_regression_tests
echo ""
green "测试完成"
