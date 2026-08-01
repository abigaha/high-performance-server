#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEST_SCRIPT="$PROJECT_ROOT/scripts/test.sh"
TEMP_DIR=$(mktemp -d)
cleanup_test() {
  local pid_file pid
  for pid_file in "$TEMP_DIR"/*-parent.pid "$TEMP_DIR"/*-child.pid; do
    [ -f "$pid_file" ] || continue
    pid="$(cat "$pid_file")"
    kill -TERM "$pid" 2>/dev/null || true
    kill -KILL "$pid" 2>/dev/null || true
  done
  rm -rf "$TEMP_DIR"
}
trap cleanup_test EXIT
umask 0022

mkdir -p "$TEMP_DIR/bin" "$TEMP_DIR/frontend/node_modules" "$TEMP_DIR/tests" "$TEMP_DIR/scripts"
cp "$TEST_SCRIPT" "$TEMP_DIR/test.sh"
mkdir -p "$TEMP_DIR/lib"
cat > "$TEMP_DIR/lib/common.sh" <<EOF
PROJECT_ROOT="$TEMP_DIR"
FRONTEND_DIR="$TEMP_DIR/frontend"
yellow() { :; }
green() { :; }
red() { :; }
ensure_frontend_dependencies() { :; }
EOF
cat > "$TEMP_DIR/bin/npm" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$CALLS"
[ -z "${E2E_BASE_URL:-}" ] || printf 'E2E_BASE_URL=%s\n' "$E2E_BASE_URL" >> "$CALLS"
[ -z "${PLAYWRIGHT_RUN_ID:-}" ] || printf 'PLAYWRIGHT_RUN_ID=%s\n' "$PLAYWRIGHT_RUN_ID" >> "$CALLS"
[ -z "${FAKE_NPM_STARTED_FILE:-}" ] || : > "$FAKE_NPM_STARTED_FILE"
if [ "${FAKE_NPM_HANG:-0}" -eq 1 ]; then
  env --default-signal=INT bash -c '
    trap '\''printf "child-int\n" > "$FAKE_NPM_CHILD_TERM_FILE"; exit 130'\'' INT
    if [ "${FAKE_NPM_IGNORE_TERM:-0}" -eq 1 ]; then
      trap "" TERM
    else
      trap '\''printf "child-term\n" > "$FAKE_NPM_CHILD_TERM_FILE"; exit 143'\'' TERM
    fi
    printf "%s\n" "$BASHPID" > "$FAKE_NPM_CHILD_PID_FILE"
    printf "ready\n" > "$FAKE_NPM_CHILD_READY_FIFO"
    IFS= read -r _ < "$FAKE_NPM_HANG_FIFO"
  ' &
  child_pid=$!
  trap 'printf "parent-int\n" > "$FAKE_NPM_PARENT_TERM_FILE"; exit 130' INT
  trap 'printf "parent-term\n" > "$FAKE_NPM_PARENT_TERM_FILE"; exit 143' TERM
  printf '%s\n' "$BASHPID" > "$FAKE_NPM_PARENT_PID_FILE"
  IFS= read -r _ < "$FAKE_NPM_CHILD_READY_FIFO"
  printf 'ready\n' > "$FAKE_NPM_READY_FIFO"
  wait "$child_pid"
fi
exit "${FAKE_NPM_RC:-0}"
EOF
cat > "$TEMP_DIR/bin/rm" <<'EOF'
#!/usr/bin/env bash
printf 'rm' >> "$CALLS"
printf ' <%s>' "$@" >> "$CALLS"
printf '\n' >> "$CALLS"
if [ "${FAKE_RM_RC:-0}" -ne 0 ] && [ "${1:-}" = "-f" ] && [[ "${2:-}" == *.env ]]; then
  exit "$FAKE_RM_RC"
fi
exec /usr/bin/rm "$@"
EOF
cat > "$TEMP_DIR/bin/xmake" <<'EOF'
#!/usr/bin/env bash
printf 'xmake %s\n' "$*" >> "$CALLS"
EOF
chmod +x "$TEMP_DIR/bin/npm" "$TEMP_DIR/bin/rm" "$TEMP_DIR/bin/xmake"
cat > "$TEMP_DIR/bin/openssl" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  "rand -hex 4") printf 'a1b2c3d4\n' ;;
  *)
    [ "${FAKE_OPENSSL_RC:-0}" -eq 0 ] || exit "$FAKE_OPENSSL_RC"
    printf '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n'
    ;;
esac
EOF
cat > "$TEMP_DIR/scripts/docker.sh" <<'EOF'
#!/usr/bin/env bash
printf 'docker.sh' >> "$CALLS"
printf ' <%s>' "$@" >> "$CALLS"
printf '\n' >> "$CALLS"
if [ "${1:-}" = "down" ] && [ -n "${FAKE_DOWN_READY_FIFO:-}" ]; then
  printf 'down-ready\n' > "$FAKE_DOWN_READY_FIFO"
  IFS= read -r _ < "$FAKE_DOWN_RELEASE_FIFO"
fi
if [ "${1:-}" = "down" ] && [ "${FAKE_DOWN_RC:-0}" -ne 0 ]; then
  exit "$FAKE_DOWN_RC"
fi
if [ "${1:-}" = "deploy" ]; then
  [ "$(umask)" = "0022" ] || exit 94
  while [ "$#" -gt 0 ]; do
    if [ "$1" = "--env-file" ]; then
      shift
      [ "$(stat -c '%a' "$1")" = "600" ] || exit 91
      grep -q '^AUTH_SECRET=' "$1" || exit 92
      grep -q '^ADMIN_PASSWORD=' "$1" || exit 93
      grep -q '^HPS_HTTP_PORT=0$' "$1" || exit 95
      break
    fi
    shift
  done
  [ "${FAKE_DEPLOY_RC:-0}" -eq 0 ] || exit "$FAKE_DEPLOY_RC"
elif [ "${1:-}" = "base-url" ]; then
  [ "${FAKE_BASE_URL_RC:-0}" -eq 0 ] || exit "$FAKE_BASE_URL_RC"
  printf 'http://127.0.0.1:23456\n'
fi
EOF
chmod +x "$TEMP_DIR/bin/openssl" "$TEMP_DIR/scripts/docker.sh"
for regression in test_codeql_discovery.sh test_docker_admin_env.sh test_test_script.sh; do
  printf '#!/usr/bin/env bash\nexit 0\n' > "$TEMP_DIR/tests/$regression"
  chmod +x "$TEMP_DIR/tests/$regression"
done
export PATH="$TEMP_DIR/bin:$PATH"
export CALLS="$TEMP_DIR/calls"

assert_entry_installs_dependencies_before_script_regression() {
  local case_name="$1"
  shift
  local case_root="$TEMP_DIR/$case_name"
  local calls_file="$case_root/calls"
  local output_log="$case_root/output.log"
  local rc=0 dependency_line regression_line

  mkdir -p "$case_root/bin" "$case_root/frontend" "$case_root/home" "$case_root/scripts/lib" \
    "$case_root/tests" "$case_root/tmp"
  cp "$TEST_SCRIPT" "$case_root/scripts/test.sh"
  cat > "$case_root/scripts/lib/common.sh" <<EOF
PROJECT_ROOT="$case_root"
FRONTEND_DIR="\$PROJECT_ROOT/frontend"
yellow() { :; }
green() { :; }
red() { :; }
ensure_frontend_dependencies() {
  printf 'ensure_frontend_dependencies\n' >> "\$TEST_ENTRY_CALLS"
  mkdir -p "\$FRONTEND_DIR/node_modules"
  : > "\$FRONTEND_DIR/node_modules/.ready"
}
EOF
  cat > "$case_root/bin/xmake" <<'EOF'
#!/usr/bin/env bash
printf 'xmake %s\n' "$*" >> "$TEST_ENTRY_CALLS"
EOF
  cat > "$case_root/bin/npm" <<'EOF'
#!/usr/bin/env bash
[ -f "$PWD/node_modules/.ready" ] || {
  printf 'npm-before-dependencies\n' >> "$TEST_ENTRY_CALLS"
  exit 72
}
printf 'npm %s\n' "$*" >> "$TEST_ENTRY_CALLS"
EOF
  chmod +x "$case_root/bin/xmake" "$case_root/bin/npm"
  for regression in test_codeql_discovery.sh test_docker_admin_env.sh; do
    printf '#!/usr/bin/env bash\nexit 0\n' > "$case_root/tests/$regression"
    chmod +x "$case_root/tests/$regression"
  done
  cat > "$case_root/tests/test_test_script.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

case_root="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$case_root/frontend/node_modules/.ready" ] || {
  printf 'playwright-cli-before-dependencies\n' >> "$TEST_ENTRY_CALLS"
  exit 73
}
printf 'playwright-cli-regression\n' >> "$TEST_ENTRY_CALLS"
EOF
  chmod +x "$case_root/tests/test_test_script.sh"

  [ ! -e "$case_root/frontend/node_modules" ] || {
    echo "$case_name 初始状态不应存在 node_modules" >&2
    exit 1
  }
  : > "$calls_file"
  set +e
  HOME="$case_root/home" TMPDIR="$case_root/tmp" PATH="$case_root/bin:$PATH" \
    TEST_ENTRY_CALLS="$calls_file" bash "$case_root/scripts/test.sh" "$@" >"$output_log" 2>&1
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$case_name 在无 node_modules 隔离入口中失败，完整日志如下:" >&2
    while IFS= read -r line || [ -n "$line" ]; do printf '%s\n' "$line" >&2; done < "$output_log"
    exit 1
  fi
  dependency_line="$(grep -n '^ensure_frontend_dependencies$' "$calls_file" | cut -d: -f1)"
  regression_line="$(grep -n '^playwright-cli-regression$' "$calls_file" | cut -d: -f1)"
  [ "$dependency_line" -lt "$regression_line" ] || {
    echo "$case_name 在依赖准备前执行了 Playwright CLI 回归" >&2
    exit 1
  }
}

assert_entry_installs_dependencies_before_script_regression backend-target test_test_script
assert_entry_installs_dependencies_before_script_regression full-entry

assert_e2e_documentation_is_canonical() {
  if grep -Fq 'npm run test:e2e' "$PROJECT_ROOT/README.md"; then
    echo '根 README 不得直接调用 npm run test:e2e' >&2
    exit 1
  fi
  grep -Fx 'bash scripts/test.sh e2e <spec>' "$PROJECT_ROOT/README.md" >/dev/null || {
    echo '根 README 必须指向唯一受控 E2E 入口' >&2
    exit 1
  }
  if grep -Eq '四个.*(视口|Playwright)|`4/4`' "$PROJECT_ROOT/README.md"; then
    echo '根 README 不得保留旧四视口描述' >&2
    exit 1
  fi
  grep -Fq '`/users` 是按角色跳转的兼容入口：`ADMIN` 到 `/admin/users`，其他已登录角色到 `/files`。' \
    "$PROJECT_ROOT/frontend/README.md" >/dev/null || {
    echo 'frontend README 未说明 /users 的真实角色跳转' >&2
    exit 1
  }
  grep -Fq '受控入口会忽略调用者提供的 `PLAYWRIGHT_RUN_ID`，并自行生成唯一运行标识。' \
    "$PROJECT_ROOT/README.md" >/dev/null || {
    echo '根 README 未说明受控入口覆盖 PLAYWRIGHT_RUN_ID' >&2
    exit 1
  }
  grep -Fq '受控入口会忽略调用者提供的 `PLAYWRIGHT_RUN_ID`，并自行生成唯一运行标识。' \
    "$PROJECT_ROOT/frontend/README.md" >/dev/null || {
    echo 'frontend README 未说明受控入口覆盖 PLAYWRIGHT_RUN_ID' >&2
    exit 1
  }
  grep -Fq '受控 E2E 的 `test-results/` 和 `playwright-report/` 中每次运行目录均形如 `e2e_YYYYMMDD_HHMMSS_YYYYMMDD_HHMMSS_<pid>_<random>`。' \
    "$PROJECT_ROOT/frontend/README.md" >/dev/null || {
    echo 'frontend README 未说明受控 E2E 双时间戳产物目录格式' >&2
    exit 1
  }
  grep -Fq '前一个时间戳由 Playwright 配置生成；后一个时间戳、`<pid>` 和 `<random>` 来自脚本生成的 `run_id`。' \
    "$PROJECT_ROOT/frontend/README.md" >/dev/null || {
    echo 'frontend README 未说明配置时间戳与脚本 run_id 的目录构成' >&2
    exit 1
  }
  if grep -Fq '由配置写为 `e2e_YYYYMMDD_HHMMSS`' "$PROJECT_ROOT/frontend/README.md"; then
    echo 'frontend README 不得将受控 E2E 目录描述为单一时间戳' >&2
    exit 1
  fi
  grep -Fq '禁止建立或引用无时间戳的 `latest` 别名。' "$PROJECT_ROOT/frontend/README.md" >/dev/null || {
    echo 'frontend README 必须禁止 latest 产物别名' >&2
    exit 1
  }
}

assert_e2e_documentation_is_canonical

assert_playwright_rejects_latest_root() {
  local variable_name="$1"
  local rejected_root="${2:-LATEST}"
  local output_log="$TEMP_DIR/playwright-$variable_name.log"
  local output_root="$TEMP_DIR/playwright-results"
  local report_root="$TEMP_DIR/playwright-report"
  if [ "$variable_name" = "PLAYWRIGHT_OUTPUT_ROOT" ]; then
    output_root="$rejected_root"
  else
    report_root="$rejected_root"
  fi
  if env E2E_RUN_ID=playwright_config_test PLAYWRIGHT_OUTPUT_ROOT="$output_root" \
    PLAYWRIGHT_REPORT_ROOT="$report_root" \
    node "$PROJECT_ROOT/frontend/node_modules/@playwright/test/cli.js" test --list \
      --config "$PROJECT_ROOT/frontend/playwright.config.ts" >"$output_log" 2>&1; then
    echo "$variable_name 含 LATEST 时 Playwright 配置必须拒绝" >&2
    exit 1
  fi
  grep -F "环境变量 $variable_name 不允许包含 latest 路径段" "$output_log"
}

assert_playwright_rejects_latest_root PLAYWRIGHT_OUTPUT_ROOT
assert_playwright_rejects_latest_root PLAYWRIGHT_REPORT_ROOT
assert_playwright_rejects_latest_root PLAYWRIGHT_OUTPUT_ROOT "$TEMP_DIR/playwright-results/nested/LATEST/child"
assert_playwright_rejects_latest_root PLAYWRIGHT_REPORT_ROOT "$TEMP_DIR/playwright-report/nested/latest/child"

assert_playwright_sanitizes_latest_run_id() {
  local output_log="$TEMP_DIR/playwright-run-id.log"
  if ! env E2E_RUN_ID=playwright_config_test \
    PLAYWRIGHT_RUN_ID='review-LATEST-candidate' \
    PLAYWRIGHT_OUTPUT_ROOT="$TEMP_DIR/playwright-results" \
    PLAYWRIGHT_REPORT_ROOT="$TEMP_DIR/playwright-report" \
    node "$PROJECT_ROOT/frontend/node_modules/@playwright/test/cli.js" test --list \
      --config "$PROJECT_ROOT/frontend/playwright.config.ts" \
    >"$output_log" 2>&1; then
    printf '%s\n' '含 latest 的 PLAYWRIGHT_RUN_ID 经 Playwright 配置加载失败，完整日志如下:' >&2
    while IFS= read -r line || [ -n "$line" ]; do printf '%s\n' "$line" >&2; done < "$output_log"
    exit 1
  fi
}

assert_playwright_sanitizes_latest_run_id

playwright_list_log="$TEMP_DIR/playwright-safe-roots.log"
if ! env E2E_RUN_ID=playwright_config_test \
  PLAYWRIGHT_OUTPUT_ROOT="$TEMP_DIR/playwright-results" \
  PLAYWRIGHT_REPORT_ROOT="$TEMP_DIR/playwright-report" \
  node "$PROJECT_ROOT/frontend/node_modules/@playwright/test/cli.js" test --list \
    --config "$PROJECT_ROOT/frontend/playwright.config.ts" >"$playwright_list_log" 2>&1; then
  printf '%s\n' '安全 Playwright 根路径的配置列举失败，完整日志如下:' >&2
  while IFS= read -r line || [ -n "$line" ]; do printf '%s\n' "$line" >&2; done < "$playwright_list_log"
  exit 1
fi

: > "$CALLS"
bash "$TEMP_DIR/test.sh" frontend tests/pages/ProfilePage.test.tsx tests/api/vip.test.ts
grep -Fx 'run test -- tests/pages/ProfilePage.test.tsx tests/api/vip.test.ts' "$CALLS"
if grep -q '^xmake ' "$CALLS"; then
  echo 'frontend 子命令不应调用 xmake' >&2
  exit 1
fi

: > "$CALLS"
bash "$TEMP_DIR/test.sh"
grep -Fx 'xmake test -j1' "$CALLS"
grep -Fx 'run test --' "$CALLS"

: > "$CALLS"
if bash "$TEMP_DIR/test.sh" invalid-target; then
  echo '非法目标必须失败' >&2
  exit 1
fi

invalid_frontend_args=(
  --watch
  --config
  --update-snapshots
  /tmp/tests/pages/ProfilePage.test.tsx
  tests/../package.json
  ../tests/pages/ProfilePage.test.tsx
  src/ProfilePage.test.tsx
  tests/pages/ProfilePage.ts
  tests/pages/ProfilePage.tsx
)
for invalid_arg in "${invalid_frontend_args[@]}"; do
  : > "$CALLS"
  if bash "$TEMP_DIR/test.sh" frontend "$invalid_arg"; then
    echo "非法 frontend 参数必须失败: $invalid_arg" >&2
    exit 1
  fi
  if [ -s "$CALLS" ]; then
    echo "非法 frontend 参数不得调用 npm/xmake: $invalid_arg" >&2
    exit 1
  fi
done

: > "$CALLS"
PLAYWRIGHT_LOG="$TEMP_DIR/playwright-success.log"
if ! PLAYWRIGHT_RUN_ID=caller_selected_run_id \
  bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts >"$PLAYWRIGHT_LOG" 2>&1; then
  printf '%s\n' 'e2e 成功路径异常退出，完整日志如下:' >&2
  while IFS= read -r line || [ -n "$line" ]; do printf '%s\n' "$line" >&2; done < "$PLAYWRIGHT_LOG"
  exit 1
fi
grep -Fx 'run test:e2e -- tests/e2e/user-governance.spec.ts' "$CALLS"
grep -Fx 'E2E_BASE_URL=http://127.0.0.1:23456' "$CALLS"
if grep -q '^xmake ' "$CALLS"; then
  echo 'e2e 子命令不应调用 xmake' >&2
  exit 1
fi
deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
project_name="$(printf '%s\n' "$deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
env_file="$(printf '%s\n' "$deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
[[ "$project_name" =~ ^hps_e2e_[0-9]{8}_[0-9]{6}_[0-9]+_a1b2c3d4$ ]] || {
  echo "e2e project 名称格式错误: $project_name" >&2
  exit 1
}
run_id="${project_name#hps_e2e_}"
grep -Fx "PLAYWRIGHT_RUN_ID=$run_id" "$CALLS"
if grep -Fx 'PLAYWRIGHT_RUN_ID=caller_selected_run_id' "$CALLS"; then
  echo '受控 e2e 入口不得向 Playwright 传递调用者指定的 RUN_ID' >&2
  exit 1
fi
[ -n "$env_file" ] || { echo 'e2e 未传 --env-file' >&2; exit 1; }
[ ! -e "$env_file" ] || { echo 'e2e 成功后未删除临时凭据' >&2; exit 1; }
grep -Fx "docker.sh <down> <--project-name> <$project_name> <--env-file> <$env_file> <--volumes>" "$CALLS"
grep -Fx "docker.sh <base-url> <--project-name> <$project_name> <--env-file> <$env_file>" "$CALLS"

: > "$CALLS"
SNAPSHOT_PLAYWRIGHT_LOG="$TEMP_DIR/playwright-snapshot-update.log"
if ! bash "$TEMP_DIR/test.sh" e2e --update-snapshots tests/e2e/user-governance.spec.ts \
  >"$SNAPSHOT_PLAYWRIGHT_LOG" 2>&1; then
  printf '%s\n' 'e2e 快照更新路径异常退出，完整日志如下:' >&2
  while IFS= read -r line || [ -n "$line" ]; do printf '%s\n' "$line" >&2; done < "$SNAPSHOT_PLAYWRIGHT_LOG"
  exit 1
fi
grep -Fx 'run test:e2e -- tests/e2e/user-governance.spec.ts --update-snapshots' "$CALLS"

: > "$CALLS"
if FAKE_DOWN_RC=42 FAKE_RM_RC=43 bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts \
  >"$TEMP_DIR/down-failure-after-success.log" 2>&1; then
  echo 'Playwright 成功但 down 或临时 env 清理失败时脚本必须失败' >&2
  exit 1
else
  rc=$?
fi
[ "$rc" -eq 42 ] || { echo "down 失败码应优先返回 42，实际为 $rc" >&2; exit 1; }
down_failure_success_deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
down_failure_success_project="$(printf '%s\n' "$down_failure_success_deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
down_failure_success_env="$(printf '%s\n' "$down_failure_success_deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
[ "$(grep -Fxc "docker.sh <down> <--project-name> <$down_failure_success_project> <--env-file> <$down_failure_success_env> <--volumes>" "$CALLS")" -eq 1 ] || {
  echo 'Playwright 成功且 down 失败时未恰好清理一次' >&2
  exit 1
}
[ -e "$down_failure_success_env" ] || { echo 'fake rm 失败后应保留临时 env 供回归确认' >&2; exit 1; }
grep -Fx '错误: E2E 清理失败: docker down 退出码 42' "$TEMP_DIR/down-failure-after-success.log"
grep -Fx '错误: E2E 清理失败: 临时 env 删除退出码 43' "$TEMP_DIR/down-failure-after-success.log"
grep -Fx "rm <-f> <$down_failure_success_env>" "$CALLS"
/usr/bin/rm -f "$down_failure_success_env"

: > "$CALLS"
if FAKE_RM_RC=43 bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts \
  >"$TEMP_DIR/rm-failure-after-success.log" 2>&1; then
  echo 'Playwright 成功但临时 env 删除失败时脚本必须失败' >&2
  exit 1
else
  rc=$?
fi
[ "$rc" -eq 43 ] || { echo "临时 env 删除失败码应返回 43，实际为 $rc" >&2; exit 1; }
rm_failure_deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
rm_failure_project="$(printf '%s\n' "$rm_failure_deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
rm_failure_env="$(printf '%s\n' "$rm_failure_deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
grep -Fx "docker.sh <down> <--project-name> <$rm_failure_project> <--env-file> <$rm_failure_env> <--volumes>" "$CALLS"
grep -Fx "rm <-f> <$rm_failure_env>" "$CALLS"
grep -Fx '错误: E2E 清理失败: 临时 env 删除退出码 43' "$TEMP_DIR/rm-failure-after-success.log"
[ -e "$rm_failure_env" ] || { echo 'fake rm 失败后应保留临时 env 供回归确认' >&2; exit 1; }
/usr/bin/rm -f "$rm_failure_env"

: > "$CALLS"
if FAKE_DEPLOY_RC=29 bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts >"$TEMP_DIR/deploy-failure.log" 2>&1; then
  echo 'Docker deploy 失败必须向上传递' >&2
  exit 1
else
  rc=$?
fi
if [ "$rc" -ne 29 ]; then
  echo "Docker deploy 失败码被改写为 $rc，完整日志如下:" >&2
  while IFS= read -r line || [ -n "$line" ]; do printf '%s\n' "$line" >&2; done < "$TEMP_DIR/deploy-failure.log"
  exit 1
fi
deploy_failure_call="$(grep '^docker.sh <deploy>' "$CALLS")"
deploy_failure_project="$(printf '%s\n' "$deploy_failure_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
deploy_failure_env="$(printf '%s\n' "$deploy_failure_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
grep -Fx "docker.sh <down> <--project-name> <$deploy_failure_project> <--env-file> <$deploy_failure_env> <--volumes>" "$CALLS"
[ ! -e "$deploy_failure_env" ] || { echo 'Docker deploy 失败后未删除临时凭据' >&2; exit 1; }

: > "$CALLS"
if FAKE_BASE_URL_RC=31 bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts >"$TEMP_DIR/base-url-failure.log" 2>&1; then
  echo '动态端口查询失败必须向上传递' >&2
  exit 1
else
  rc=$?
fi
[ "$rc" -eq 31 ] || { echo "动态端口查询失败码被改写为 $rc" >&2; exit 1; }
base_url_failure_deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
base_url_failure_project="$(printf '%s\n' "$base_url_failure_deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
base_url_failure_env="$(printf '%s\n' "$base_url_failure_deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
grep -Fx "docker.sh <base-url> <--project-name> <$base_url_failure_project> <--env-file> <$base_url_failure_env>" "$CALLS"
grep -Fx "docker.sh <down> <--project-name> <$base_url_failure_project> <--env-file> <$base_url_failure_env> <--volumes>" "$CALLS"
[ ! -e "$base_url_failure_env" ] || { echo '动态端口查询失败后未删除临时凭据' >&2; exit 1; }
if grep -q '^run ' "$CALLS"; then
  echo '动态端口查询失败后不应运行 Playwright' >&2
  exit 1
fi

mkdir "$TEMP_DIR/random-failure-tmp"
: > "$CALLS"
if TMPDIR="$TEMP_DIR/random-failure-tmp" FAKE_OPENSSL_RC=41 \
  bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts >"$TEMP_DIR/random-failure.log" 2>&1; then
  echo '凭据随机数生成失败必须向上传递' >&2
  exit 1
else
  rc=$?
fi
[ "$rc" -eq 41 ] || { echo "凭据随机数失败码被改写为 $rc" >&2; exit 1; }
if compgen -G "$TEMP_DIR/random-failure-tmp/hps_e2e_*.env" >/dev/null; then
  echo '凭据随机数生成失败后遗留临时 env' >&2
  exit 1
fi
grep -q '^docker.sh <down> .* <--volumes>$' "$CALLS" || { echo '凭据准备失败后未清理隔离 project/volume' >&2; exit 1; }
if grep -q '^docker.sh <deploy>\|^run \|^xmake ' "$CALLS"; then
  echo '凭据准备失败不应调用 deploy/npm/xmake' >&2
  exit 1
fi

: > "$CALLS"
if FAKE_NPM_RC=37 bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts >"$TEMP_DIR/playwright-failure.log" 2>&1; then
  echo 'Playwright 失败码必须向上传递' >&2
  exit 1
else
  rc=$?
fi
[ "$rc" -eq 37 ] || { echo "Playwright 失败码被改写为 $rc" >&2; exit 1; }
failure_deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
failure_project="$(printf '%s\n' "$failure_deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
failure_env="$(printf '%s\n' "$failure_deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
grep -Fx "docker.sh <down> <--project-name> <$failure_project> <--env-file> <$failure_env> <--volumes>" "$CALLS"
[ ! -e "$failure_env" ] || { echo 'e2e 失败后未删除临时凭据' >&2; exit 1; }

: > "$CALLS"
if FAKE_NPM_RC=37 FAKE_DOWN_RC=42 FAKE_RM_RC=43 bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts \
  >"$TEMP_DIR/playwright-and-down-failure.log" 2>&1; then
  echo 'Playwright、down 与临时 env 删除同时失败时必须失败' >&2
  exit 1
else
  rc=$?
fi
[ "$rc" -eq 37 ] || { echo "Playwright 失败码应优先保留 37，实际为 $rc" >&2; exit 1; }
combined_failure_deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
combined_failure_project="$(printf '%s\n' "$combined_failure_deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
combined_failure_env="$(printf '%s\n' "$combined_failure_deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
[ "$(grep -Fxc "docker.sh <down> <--project-name> <$combined_failure_project> <--env-file> <$combined_failure_env> <--volumes>" "$CALLS")" -eq 1 ] || {
  echo 'Playwright 与 down 同时失败时未恰好清理一次' >&2
  exit 1
}
[ -e "$combined_failure_env" ] || { echo 'fake rm 失败后应保留组合失败路径临时 env 供回归确认' >&2; exit 1; }
grep -Fx '错误: E2E 清理失败: docker down 退出码 42' "$TEMP_DIR/playwright-and-down-failure.log"
grep -Fx '错误: E2E 清理失败: 临时 env 删除退出码 43' "$TEMP_DIR/playwright-and-down-failure.log"
grep -Fx "rm <-f> <$combined_failure_env>" "$CALLS"
/usr/bin/rm -f "$combined_failure_env"

: > "$CALLS"
FAKE_NPM_RC=143 FAKE_RM_RC=43 bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts >"$TEMP_DIR/playwright-signal.log" 2>&1 &
signal_pid=$!
wait "$signal_pid" || signal_rc=$?
[ "${signal_rc:-0}" -eq 143 ] || { echo "信号失败码被改写为 ${signal_rc:-0}" >&2; exit 1; }
signal_deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
signal_project="$(printf '%s\n' "$signal_deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
signal_env="$(printf '%s\n' "$signal_deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
grep -Fx "docker.sh <down> <--project-name> <$signal_project> <--env-file> <$signal_env> <--volumes>" "$CALLS"
grep -Fx "rm <-f> <$signal_env>" "$CALLS"
grep -Fx '错误: E2E 清理失败: 临时 env 删除退出码 43' "$TEMP_DIR/playwright-signal.log"
[ -e "$signal_env" ] || { echo 'fake rm 失败后应保留信号路径临时凭据供回归确认' >&2; exit 1; }
/usr/bin/rm -f "$signal_env"

: > "$CALLS"
actual_signal_ready_fifo="$TEMP_DIR/actual-signal.ready"
actual_signal_child_ready_fifo="$TEMP_DIR/actual-signal-child.ready"
actual_signal_hang_fifo="$TEMP_DIR/actual-signal.hang"
mkfifo "$actual_signal_ready_fifo" "$actual_signal_child_ready_fifo" "$actual_signal_hang_fifo"
actual_signal_parent_pid_file="$TEMP_DIR/actual-signal-parent.pid"
actual_signal_child_pid_file="$TEMP_DIR/actual-signal-child.pid"
actual_signal_parent_term_file="$TEMP_DIR/actual-signal-parent.term"
actual_signal_child_term_file="$TEMP_DIR/actual-signal-child.term"
FAKE_NPM_HANG=1 \
  FAKE_RM_RC=43 \
  FAKE_NPM_READY_FIFO="$actual_signal_ready_fifo" \
  FAKE_NPM_CHILD_READY_FIFO="$actual_signal_child_ready_fifo" \
  FAKE_NPM_HANG_FIFO="$actual_signal_hang_fifo" \
  FAKE_NPM_PARENT_PID_FILE="$actual_signal_parent_pid_file" \
  FAKE_NPM_CHILD_PID_FILE="$actual_signal_child_pid_file" \
  FAKE_NPM_PARENT_TERM_FILE="$actual_signal_parent_term_file" \
  FAKE_NPM_CHILD_TERM_FILE="$actual_signal_child_term_file" \
  bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts >"$TEMP_DIR/actual-signal.log" 2>&1 &
actual_signal_pid=$!
IFS= read -r actual_signal_event < "$actual_signal_ready_fifo"
[ "$actual_signal_event" = "ready" ] || { echo '真实信号回归未进入 Playwright 阶段' >&2; exit 1; }
actual_npm_parent_pid="$(cat "$actual_signal_parent_pid_file")"
actual_npm_child_pid="$(cat "$actual_signal_child_pid_file")"
kill -TERM "$actual_signal_pid"
timeout 5s tail --pid="$actual_signal_pid" -f /dev/null || { echo 'TERM 后 test.sh 未限时退出' >&2; exit 1; }
wait "$actual_signal_pid" || actual_signal_rc=$?
[ "${actual_signal_rc:-0}" -eq 143 ] || { echo "TERM 退出码被改写为 ${actual_signal_rc:-0}" >&2; exit 1; }
[ "$(cat "$actual_signal_parent_term_file")" = "parent-term" ] || { echo 'TERM 未转发给 fake npm 父进程' >&2; exit 1; }
[ "$(cat "$actual_signal_child_term_file")" = "child-term" ] || { echo 'TERM 未转发给 fake npm 子进程' >&2; exit 1; }
if kill -0 "$actual_npm_parent_pid" 2>/dev/null || kill -0 "$actual_npm_child_pid" 2>/dev/null; then
  echo 'TERM 后 fake npm 父子进程仍存活' >&2
  exit 1
fi
actual_signal_deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
actual_signal_project="$(printf '%s\n' "$actual_signal_deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
actual_signal_env="$(printf '%s\n' "$actual_signal_deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
grep -Fx "docker.sh <down> <--project-name> <$actual_signal_project> <--env-file> <$actual_signal_env> <--volumes>" "$CALLS"
grep -Fx "rm <-f> <$actual_signal_env>" "$CALLS"
grep -Fx '错误: E2E 清理失败: 临时 env 删除退出码 43' "$TEMP_DIR/actual-signal.log"
[ -e "$actual_signal_env" ] || { echo 'fake rm 失败后应保留 TERM 路径临时凭据供回归确认' >&2; exit 1; }
/usr/bin/rm -f "$actual_signal_env"

run_term_ignoring_child_case() {
  local ready_fifo="$TEMP_DIR/term-ignoring-child.ready"
  local child_ready_fifo="$TEMP_DIR/term-ignoring-child-child.ready"
  local hang_fifo="$TEMP_DIR/term-ignoring-child.hang"
  local parent_pid_file="$TEMP_DIR/term-ignoring-child-parent.pid"
  local child_pid_file="$TEMP_DIR/term-ignoring-child-child.pid"
  local parent_term_file="$TEMP_DIR/term-ignoring-child-parent.term"
  : > "$CALLS"
  mkfifo "$ready_fifo" "$child_ready_fifo" "$hang_fifo"

  FAKE_NPM_HANG=1 \
    FAKE_NPM_IGNORE_TERM=1 \
    FAKE_NPM_READY_FIFO="$ready_fifo" \
    FAKE_NPM_CHILD_READY_FIFO="$child_ready_fifo" \
    FAKE_NPM_HANG_FIFO="$hang_fifo" \
    FAKE_NPM_PARENT_PID_FILE="$parent_pid_file" \
    FAKE_NPM_CHILD_PID_FILE="$child_pid_file" \
    FAKE_NPM_PARENT_TERM_FILE="$parent_term_file" \
    env --default-signal=INT bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts \
      >"$TEMP_DIR/term-ignoring-child.log" 2>&1 &
  local script_pid=$!
  local event
  IFS= read -r event < "$ready_fifo"
  [ "$event" = "ready" ] || { echo '忽略 TERM 子进程回归未进入 Playwright 阶段' >&2; exit 1; }
  local npm_child_pid
  npm_child_pid="$(cat "$child_pid_file")"

  kill -TERM "$script_pid"
  timeout 5s tail --pid="$script_pid" -f /dev/null || { echo '忽略 TERM 子进程时 test.sh 未限时退出' >&2; exit 1; }
  local script_rc=0
  wait "$script_pid" || script_rc=$?
  [ "$script_rc" -eq 143 ] || { echo "忽略 TERM 子进程时退出码应为 143，实际为 $script_rc" >&2; exit 1; }
  [ "$(cat "$parent_term_file")" = "parent-term" ] || { echo 'TERM 未转发给忽略 TERM 子进程的 fake npm 组长' >&2; exit 1; }
  if kill -0 "$npm_child_pid" 2>/dev/null; then
    echo 'TERM grace 超时后 fake npm 忽略 TERM 子进程仍存活' >&2
    exit 1
  fi

  local deploy_call project env_file
  deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
  project="$(printf '%s\n' "$deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
  env_file="$(printf '%s\n' "$deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
  grep -Fx "docker.sh <down> <--project-name> <$project> <--env-file> <$env_file> <--volumes>" "$CALLS"
  grep -Fx "rm <-f> <$env_file>" "$CALLS"
}

run_term_ignoring_child_case

run_repeated_signal_case() {
  local first_signal="$1" second_signal="$2" expected_rc="$3" case_name="$4"
  local ready_fifo="$TEMP_DIR/$case_name.ready"
  local child_ready_fifo="$TEMP_DIR/$case_name-child.ready"
  local hang_fifo="$TEMP_DIR/$case_name.hang"
  local down_ready_fifo="$TEMP_DIR/$case_name-down.ready"
  local down_release_fifo="$TEMP_DIR/$case_name-down.release"
  mkfifo "$ready_fifo" "$child_ready_fifo" "$hang_fifo" "$down_ready_fifo" "$down_release_fifo"
  local parent_pid_file="$TEMP_DIR/$case_name-parent.pid"
  local child_pid_file="$TEMP_DIR/$case_name-child.pid"
  local parent_term_file="$TEMP_DIR/$case_name-parent.term"
  local child_term_file="$TEMP_DIR/$case_name-child.term"
  : > "$CALLS"

  FAKE_NPM_HANG=1 \
    FAKE_NPM_READY_FIFO="$ready_fifo" \
    FAKE_NPM_CHILD_READY_FIFO="$child_ready_fifo" \
    FAKE_NPM_HANG_FIFO="$hang_fifo" \
    FAKE_NPM_PARENT_PID_FILE="$parent_pid_file" \
    FAKE_NPM_CHILD_PID_FILE="$child_pid_file" \
    FAKE_NPM_PARENT_TERM_FILE="$parent_term_file" \
    FAKE_NPM_CHILD_TERM_FILE="$child_term_file" \
    FAKE_DOWN_READY_FIFO="$down_ready_fifo" \
    FAKE_DOWN_RELEASE_FIFO="$down_release_fifo" \
    FAKE_DOWN_RC=42 \
    env --default-signal=INT bash "$TEMP_DIR/test.sh" e2e tests/e2e/user-governance.spec.ts \
      >"$TEMP_DIR/$case_name.log" 2>&1 &
  local script_pid=$!
  local event
  IFS= read -r event < "$ready_fifo"
  [ "$event" = "ready" ] || { echo "$case_name 未进入 Playwright 阶段" >&2; exit 1; }
  local npm_parent_pid npm_child_pid
  npm_parent_pid="$(cat "$parent_pid_file")"
  npm_child_pid="$(cat "$child_pid_file")"

  kill -s "$first_signal" "$script_pid"
  IFS= read -r event < "$down_ready_fifo"
  [ "$event" = "down-ready" ] || { echo "$case_name 未进入 cleanup down" >&2; exit 1; }
  kill -s "$second_signal" "$script_pid"
  printf 'release\n' > "$down_release_fifo"
  timeout 5s tail --pid="$script_pid" -f /dev/null || { echo "$case_name 重复信号后未限时退出" >&2; exit 1; }
  local script_rc=0
  wait "$script_pid" || script_rc=$?
  [ "$script_rc" -eq "$expected_rc" ] || {
    echo "$case_name 首次信号退出码应为 $expected_rc，实际为 $script_rc" >&2
    exit 1
  }
  if kill -0 "$npm_parent_pid" 2>/dev/null || kill -0 "$npm_child_pid" 2>/dev/null; then
    echo "$case_name 后 fake npm 子进程树仍存活" >&2
    exit 1
  fi
  local deploy_call project env
  deploy_call="$(grep '^docker.sh <deploy>' "$CALLS")"
  project="$(printf '%s\n' "$deploy_call" | grep -o '<--project-name> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
  env="$(printf '%s\n' "$deploy_call" | grep -o '<--env-file> <[^>]*>' | cut -d'<' -f3 | tr -d '>')"
  [ "$(grep -Fxc "docker.sh <down> <--project-name> <$project> <--env-file> <$env> <--volumes>" "$CALLS")" -eq 1 ] || {
    echo "$case_name 未恰好执行一次 down --volumes" >&2
    exit 1
  }
  [ ! -e "$env" ] || { echo "$case_name 后未删除临时 env" >&2; exit 1; }
  grep -Fx '错误: E2E 清理失败: docker down 退出码 42' "$TEMP_DIR/$case_name.log" >/dev/null || {
    echo "$case_name 未报告 down 清理失败" >&2
    exit 1
  }
}

run_repeated_signal_case TERM TERM 143 repeated-term
run_repeated_signal_case INT TERM 130 int-then-term

e2e_secrets=(
  0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
  E2eAdmin_a1b2c3d4_0123456789abcdef0123456789abcdef0123456789abcdef
)
for secret in "${e2e_secrets[@]}"; do
  if grep -Fq "$secret" "$CALLS" "$TEMP_DIR"/*.log 2>/dev/null; then
    echo 'e2e 日志或调用记录泄露凭据' >&2
    exit 1
  fi
done

invalid_e2e_args=(
  --headed
  --config
  /tmp/tests/e2e/user-governance.spec.ts
  tests/e2e/../api/client.test.ts
  ../tests/e2e/user-governance.spec.ts
  tests/api/client.test.ts
  'tests/e2e/user-governance.spec.ts;id'
)
for invalid_arg in "${invalid_e2e_args[@]}"; do
  : > "$CALLS"
  if bash "$TEMP_DIR/test.sh" e2e "$invalid_arg"; then
    echo "非法 e2e 参数必须失败: $invalid_arg" >&2
    exit 1
  fi
  if [ -s "$CALLS" ]; then
    echo "非法 e2e 参数不得调用 npm/xmake/docker: $invalid_arg" >&2
    exit 1
  fi
done

: > "$CALLS"
if bash "$TEMP_DIR/test.sh" e2e --update-snapshots; then
  echo 'e2e 快照更新选项缺少 spec 路径必须失败' >&2
  exit 1
fi
if [ -s "$CALLS" ]; then
  echo '缺少 spec 路径的 e2e 快照更新不得调用 npm/xmake/docker' >&2
  exit 1
fi

echo 'test.sh frontend/e2e 参数路由回归通过'
