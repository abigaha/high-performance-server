#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

fail() {
  printf '失败: %s\n' "$1" >&2
  exit 1
}

write_base_env() {
  local target="$1"
  cat > "$target" <<'EOF'
AUTH_SECRET=0123456789abcdef0123456789abcdef
MYSQL_ROOT_PASSWORD=root-password-0123456789
MYSQL_USER=hps
MYSQL_PASSWORD=app-password-0123456789
HPS_HTTP_PORT=18080
EOF
}

load_docker_library() {
  local target="$1"
  HPS_DOCKER_ENV_FILE="$target" source "$ROOT/scripts/docker.sh"
  [ "$ENV_FILE" = "$target" ] || fail "docker.sh 未使用注入的临时 env 路径"
}

generated_env="$TMP_DIR/generated.env"
load_docker_library "$generated_env"
generation_log="$TMP_DIR/generation.log"
ensure_env_file >"$generation_log" 2>&1
[ "$(stat -c '%a' "$generated_env")" = "600" ] || fail "新 env 权限不是 0600"
generated_username="$(env_file_value ADMIN_USERNAME)"
generated_password="$(env_file_value ADMIN_PASSWORD)"
generated_email="$(env_file_value ADMIN_EMAIL)"
[[ "$generated_username" =~ ^admin_[0-9a-f]{16}$ ]] || fail "管理员用户名格式错误"
[ "${#generated_password}" -ge 64 ] || fail "管理员密码不足 32 字节随机值"
[ "$generated_email" = "${generated_username}@localhost.invalid" ] || fail "管理员邮箱格式错误"
if grep -Fq "$generated_password" "$generation_log"; then
  fail "生成日志泄露管理员密码"
fi

missing_env="$TMP_DIR/missing.env"
write_base_env "$missing_env"
chmod 644 "$missing_env"
load_docker_library "$missing_env"
ensure_env_file >/dev/null
[ "$(stat -c '%a' "$missing_env")" = "600" ] || fail "已有 env 权限未收紧为 0600"
if grep -q '^ADMIN_' "$missing_env"; then
  fail "已有 env 全缺管理员项时被自动追加"
fi

empty_env="$TMP_DIR/empty.env"
write_base_env "$empty_env"
cat >> "$empty_env" <<'EOF'
ADMIN_USERNAME=
ADMIN_PASSWORD=
ADMIN_EMAIL=
EOF
load_docker_library "$empty_env"
validate_env_file >/dev/null || fail "管理员三项全空应视为禁用"

partial_env="$TMP_DIR/partial.env"
write_base_env "$partial_env"
partial_secret='must-not-appear-in-output'
{
  printf 'ADMIN_USERNAME=admin_0123456789abcdef\n'
  printf 'ADMIN_PASSWORD=%s\n' "$partial_secret"
} >> "$partial_env"
load_docker_library "$partial_env"
if validate_env_file >"$TMP_DIR/partial.log" 2>&1; then
  fail "管理员部分配置未被拒绝"
fi
if grep -Fq "$partial_secret" "$TMP_DIR/partial.log"; then
  fail "失败日志泄露管理员密码"
fi

placeholder_env="$TMP_DIR/placeholder.env"
write_base_env "$placeholder_env"
cat >> "$placeholder_env" <<'EOF'
ADMIN_USERNAME=replace-admin
ADMIN_PASSWORD=replace-with-a-secure-admin-password
ADMIN_EMAIL=replace-admin@localhost.invalid
EOF
load_docker_library "$placeholder_env"
if validate_env_file >/dev/null 2>&1; then
  fail "管理员占位配置未被拒绝"
fi

short_env="$TMP_DIR/short.env"
write_base_env "$short_env"
cat >> "$short_env" <<'EOF'
ADMIN_USERNAME=admin_0123456789abcdef
ADMIN_PASSWORD=too-short
ADMIN_EMAIL=admin_0123456789abcdef@localhost.invalid
EOF
load_docker_library "$short_env"
if validate_env_file >"$TMP_DIR/short.log" 2>&1; then
  fail "短管理员密码未被拒绝"
fi
if grep -Fq 'too-short' "$TMP_DIR/short.log"; then
  fail "校验日志泄露短管理员密码"
fi

long_email_env="$TMP_DIR/long-email.env"
write_base_env "$long_email_env"
long_email="$(printf 'a%.0s' {1..117})@example.com"
{
  printf 'ADMIN_USERNAME=admin_0123456789abcdef\n'
  printf 'ADMIN_PASSWORD=valid-admin-password-1234567890\n'
  printf 'ADMIN_EMAIL=%s\n' "$long_email"
} >> "$long_email_env"
load_docker_library "$long_email_env"
if validate_env_file >"$TMP_DIR/long-email.log" 2>&1; then
  fail "超过 128 字符的管理员邮箱未被拒绝"
fi
if grep -Fq "$long_email" "$TMP_DIR/long-email.log"; then
  fail "校验日志泄露管理员邮箱"
fi

symlink_target="$TMP_DIR/symlink-target.env"
write_base_env "$symlink_target"
symlink_env="$TMP_DIR/symlink.env"
ln -s "$symlink_target" "$symlink_env"
load_docker_library "$symlink_env"
if ensure_env_file >"$TMP_DIR/symlink.log" 2>&1; then
  fail "符号链接 env 未被拒绝"
fi
[ -L "$symlink_env" ] || fail "符号链接 env 被替换"

race_env="$TMP_DIR/race.env"
fake_bin="$TMP_DIR/bin"
mkdir "$fake_bin"
cat > "$fake_bin/openssl" <<'EOF'
#!/usr/bin/env bash
if [ ! -e "$RACE_TARGET" ]; then
  printf 'race-winner\n' > "$RACE_TARGET"
fi
exec /usr/bin/openssl "$@"
EOF
chmod 700 "$fake_bin/openssl"
load_docker_library "$race_env"
if RACE_TARGET="$race_env" PATH="$fake_bin:$PATH" ensure_env_file >"$TMP_DIR/race.log" 2>&1; then
  fail "目标竞态出现时仍覆盖安装 env"
fi
[ "$(cat "$race_env")" = "race-winner" ] || fail "目标竞态文件被覆盖"
shopt -s nullglob
race_temps=("$race_env".tmp.*)
[ "${#race_temps[@]}" -eq 0 ] || fail "目标竞态后遗留临时 env"
shopt -u nullglob

dispatcher_log="$TMP_DIR/dispatcher.log"
if HPS_DOCKER_LIB_ONLY=1 bash "$ROOT/scripts/docker.sh" definitely-unknown >"$dispatcher_log" 2>&1; then
  fail "旧 HPS_DOCKER_LIB_ONLY 绕过了直接执行 dispatcher"
fi
grep -q '未知子命令' "$dispatcher_log" || fail "直接执行未进入 dispatcher"
HPS_DOCKER_LIB_ONLY=1 bash "$ROOT/scripts/docker.sh" --help >"$dispatcher_log" 2>&1
grep -q '用法:' "$dispatcher_log" || fail "直接执行 --help 未进入 dispatcher"

compose_calls="$TMP_DIR/compose-calls.log"
fake_docker_bin="$TMP_DIR/docker-bin"
mkdir "$fake_docker_bin"
cat > "$fake_docker_bin/docker" <<'EOF'
#!/usr/bin/env bash
printf 'docker' >> "$COMPOSE_CALLS"
printf ' <%s>' "$@" >> "$COMPOSE_CALLS"
printf '\n' >> "$COMPOSE_CALLS"
case "$*" in
  "info") exit 0 ;;
  "compose version") exit 0 ;;
  *"compose"*"port nginx 80") printf '%b' "${FAKE_PORT_OUTPUT-127.0.0.1:32768\\n}"; exit 0 ;;
  *"ps --all --quiet"*) exit 0 ;;
esac
EOF
chmod 700 "$fake_docker_bin/docker"
isolated_env="$TMP_DIR/isolated-secret.env"
write_base_env "$isolated_env"
cat >> "$isolated_env" <<'EOF'
ADMIN_USERNAME=admin_isolated
ADMIN_PASSWORD=isolated-admin-password-1234567890
ADMIN_EMAIL=admin_isolated@example.com
EOF
chmod 600 "$isolated_env"
: > "$compose_calls"
COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
  bash "$ROOT/scripts/docker.sh" down --project-name hps_e2e_20260728_120000_42_a1b2c3d4 \
  --env-file "$isolated_env" --volumes >"$TMP_DIR/down.log" 2>&1
grep -Fx "docker <compose> <--project-directory> <$ROOT> <--project-name> <hps_e2e_20260728_120000_42_a1b2c3d4> <--env-file> <$isolated_env> <down> <--remove-orphans> <--timeout> <30> <--volumes>" "$compose_calls" || fail "down 未向 compose 转发隔离 project/env/volumes"
if grep -Fq 'isolated-admin-password-1234567890' "$TMP_DIR/down.log" "$compose_calls"; then
  fail "down 输出泄露管理员凭据"
fi

: > "$compose_calls"
COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
  bash "$ROOT/scripts/docker.sh" base-url --project-name hps_e2e_20260728_120000_42_a1b2c3d4 \
  --env-file "$isolated_env" >"$TMP_DIR/base-url.log" 2>&1
grep -Fx 'http://127.0.0.1:32768' "$TMP_DIR/base-url.log" || fail "base-url 未输出当前 project 的 nginx 动态映射"
grep -Fx "docker <compose> <--project-directory> <$ROOT> <--project-name> <hps_e2e_20260728_120000_42_a1b2c3d4> <--env-file> <$isolated_env> <port> <nginx> <80>" "$compose_calls" || fail "base-url 未查询当前 project 的 nginx 80 映射"

: > "$compose_calls"
COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
  bash "$ROOT/scripts/docker.sh" logs --since 10m >"$TMP_DIR/logs.log" 2>&1
grep -Fx "docker <compose> <--project-directory> <$ROOT> <--env-file> <$ROOT/.env> <logs> <--no-color> <--since> <10m>" "$compose_calls" || fail "logs --since 10m 参数未完整转发"

missing_default_env="$TMP_DIR/default-missing.env"
for control_command in status health stop down logs; do
  : > "$compose_calls"
  command_args=("$control_command")
  [ "$control_command" != down ] || command_args+=(--volumes)
  HPS_DOCKER_ENV_FILE="$missing_default_env" COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
    bash "$ROOT/scripts/docker.sh" "${command_args[@]}" >"$TMP_DIR/$control_command-missing-default.log" 2>&1 || true
  [ -s "$compose_calls" ] || fail "$control_command 在默认 env 不存在时未执行 Compose 控制命令"
  if grep -Fq '<--env-file>' "$compose_calls"; then
    fail "$control_command 在默认 env 不存在时仍传递 --env-file"
  fi
done

for explicit_command in base-url deploy down; do
  : > "$compose_calls"
  if COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
    bash "$ROOT/scripts/docker.sh" "$explicit_command" --env-file "$TMP_DIR/explicit-missing.env" \
      >"$TMP_DIR/$explicit_command-explicit-missing.log" 2>&1; then
    fail "$explicit_command 未拒绝不存在的显式 --env-file"
  fi
  [ ! -s "$compose_calls" ] || fail "$explicit_command 在显式 env 缺失时调用了 docker"
done

no_arg_commands=(status health build image --help)
for no_arg_command in "${no_arg_commands[@]}"; do
  : > "$compose_calls"
  if COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
    bash "$ROOT/scripts/docker.sh" "$no_arg_command" unexpected >"$TMP_DIR/$no_arg_command-unexpected.log" 2>&1; then
    fail "$no_arg_command 接受了 unexpected 参数"
  else
    rc=$?
  fi
  [ "$rc" -eq 2 ] || fail "$no_arg_command 拒绝 unexpected 参数时退出码不是 2"
  grep -Fx "错误: $no_arg_command 不接受额外参数: unexpected" "$TMP_DIR/$no_arg_command-unexpected.log" \
    || fail "$no_arg_command 未输出固定的额外参数错误"
  [ ! -s "$compose_calls" ] || fail "$no_arg_command 拒绝参数前调用了 docker"
done

: > "$compose_calls"
if COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
  bash "$ROOT/scripts/docker.sh" logs --since 10m unexpected >"$TMP_DIR/logs-unexpected.log" 2>&1; then
  fail "logs 接受了合法 options 后的 unexpected 参数"
fi
[ ! -s "$compose_calls" ] || fail "logs 拒绝多余参数前调用了 docker"

assert_base_url_output() {
  local name="$1" port_output="$2" expected_url="$3"
  : > "$compose_calls"
  FAKE_PORT_OUTPUT="$port_output" COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
    bash "$ROOT/scripts/docker.sh" base-url --project-name hps_e2e_20260728_120000_42_a1b2c3d4 \
      --env-file "$isolated_env" >"$TMP_DIR/base-url-$name.log" 2>&1 || fail "base-url $name 应成功"
  [ "$(cat "$TMP_DIR/base-url-$name.log")" = "$expected_url" ] || fail "base-url $name 输出不安全或不正确"
}

assert_base_url_failure() {
  local name="$1" port_output="$2"
  : > "$compose_calls"
  if FAKE_PORT_OUTPUT="$port_output" COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
    bash "$ROOT/scripts/docker.sh" base-url --project-name hps_e2e_20260728_120000_42_a1b2c3d4 \
      --env-file "$isolated_env" >"$TMP_DIR/base-url-$name.log" 2>&1; then
    fail "base-url $name 应失败"
  fi
}

assert_base_url_output ipv4 '0.0.0.0:31001\n' 'http://127.0.0.1:31001'
assert_base_url_output ipv6 '[::1]:31002\n' 'http://[::1]:31002'
assert_base_url_output selected-loopback '0.0.0.0:31003\n127.0.0.1:31004\n[::]:31003\n' 'http://127.0.0.1:31004'
assert_base_url_failure ambiguous-multiline '0.0.0.0:31005\n[::]:31005\n'
assert_base_url_failure empty ''
assert_base_url_failure nonnumeric '127.0.0.1:not-a-port\n'
assert_base_url_failure out-of-range '127.0.0.1:70000\n'

malicious_docker_args=(
  'bad/project'
  'hps_e2e_$(id)'
  '--help'
)
for malicious_arg in "${malicious_docker_args[@]}"; do
  : > "$compose_calls"
  if COMPOSE_CALLS="$compose_calls" PATH="$fake_docker_bin:$PATH" \
    bash "$ROOT/scripts/docker.sh" down --project-name "$malicious_arg" --env-file "$isolated_env" --volumes >/dev/null 2>&1; then
    fail "恶意 project name 未被拒绝: $malicious_arg"
  fi
  [ ! -s "$compose_calls" ] || fail "恶意 project name 调用了 docker: $malicious_arg"
done

printf 'Docker 管理员 env 回归测试通过\n'
