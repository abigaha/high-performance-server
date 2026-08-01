#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/common.sh"
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
  bash "$PROJECT_ROOT/tests/test_docker_admin_env.sh"
  bash "$PROJECT_ROOT/tests/test_test_script.sh"
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
  local timestamp random_suffix run_id project_name env_file e2e_base_url
  local playwright_pid="" playwright_rc previous_umask
  local admin_username admin_email admin_password normal_one_username normal_one_email normal_one_password
  local normal_two_username normal_two_email normal_two_password auth_secret mysql_root_password mysql_password
  timestamp="$(date '+%Y%m%d_%H%M%S')"
  random_suffix="$(openssl rand -hex 4)"
  run_id="${timestamp}_$$_${random_suffix}"
  project_name="hps_e2e_${run_id}"
  previous_umask="$(umask)"
  umask 077
  env_file="$(mktemp "${TMPDIR:-/tmp}/hps_e2e_${run_id}.XXXXXX.env")"
  E2E_CLEANUP_PROJECT_NAME="$project_name"
  E2E_CLEANUP_ENV_FILE="$env_file"

  cleanup_e2e() {
    local down_rc=0 rm_rc=0
    trap - EXIT
    trap '' INT TERM
    bash "$PROJECT_ROOT/scripts/docker.sh" down --project-name "$E2E_CLEANUP_PROJECT_NAME" \
      --env-file "$E2E_CLEANUP_ENV_FILE" --volumes || down_rc=$?
    rm -f "$E2E_CLEANUP_ENV_FILE" || rm_rc=$?
    if [ "$down_rc" -ne 0 ]; then
      printf '错误: E2E 清理失败: docker down 退出码 %s\n' "$down_rc" >&2
    fi
    if [ "$rm_rc" -ne 0 ]; then
      printf '错误: E2E 清理失败: 临时 env 删除退出码 %s\n' "$rm_rc" >&2
    fi
    if [ "$down_rc" -ne 0 ]; then
      return "$down_rc"
    fi
    return "$rm_rc"
  }
  forward_e2e_signal() {
    local signal="$1" signal_rc="$2" cleanup_rc=0
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
    cleanup_e2e || cleanup_rc=$?
    exit "$signal_rc"
  }
  trap 'original_rc=$?; cleanup_rc=0; cleanup_e2e || cleanup_rc=$?; if [ "$original_rc" -ne 0 ]; then exit "$original_rc"; else exit "$cleanup_rc"; fi' EXIT
  trap 'forward_e2e_signal INT 130' INT
  trap 'forward_e2e_signal TERM 143' TERM

  admin_username="admin_${run_id}"
  admin_email="${admin_username}@example.invalid"
  admin_password="E2eAdmin_${random_suffix}_$(openssl rand -hex 32)"
  normal_one_username="normal_a_${run_id}"
  normal_one_email="${normal_one_username}@example.invalid"
  normal_one_password="E2eNormalA_${random_suffix}_$(openssl rand -hex 24)"
  normal_two_username="normal_b_${run_id}"
  normal_two_email="${normal_two_username}@example.invalid"
  normal_two_password="E2eNormalB_${random_suffix}_$(openssl rand -hex 24)"
  auth_secret="$(openssl rand -hex 48)"
  mysql_root_password="$(openssl rand -hex 32)"
  mysql_password="$(openssl rand -hex 32)"

  {
    printf 'AUTH_SECRET=%s\n' "$auth_secret"
    printf 'MYSQL_ROOT_PASSWORD=%s\n' "$mysql_root_password"
    printf 'MYSQL_USER=hps\n'
    printf 'MYSQL_PASSWORD=%s\n' "$mysql_password"
    printf 'HPS_HTTP_PORT=0\n'
    printf 'ADMIN_USERNAME=%s\n' "$admin_username"
    printf 'ADMIN_PASSWORD=%s\n' "$admin_password"
    printf 'ADMIN_EMAIL=%s\n' "$admin_email"
  } > "$env_file"
  umask "$previous_umask"

  yellow "=== 隔离 Playwright E2E: $run_id ==="
  bash "$PROJECT_ROOT/scripts/docker.sh" deploy --project-name "$project_name" --env-file "$env_file"
  e2e_base_url="$(bash "$PROJECT_ROOT/scripts/docker.sh" base-url --project-name "$project_name" \
    --env-file "$env_file")"
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
  cleanup_rc=0
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
