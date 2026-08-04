#!/usr/bin/env bash
# ============================================================
# isolated_docker_env.sh — 可复用的隔离 Docker 测试环境生命周期
# 用法: source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/isolated_docker_env.sh"
#
# 导出:
#   hps_start_isolated_environment <prefix> <run_id>
#   hps_cleanup_isolated_environment
#   hps_runtime_fingerprint
# ============================================================

if [ -n "${__ISOLATED_DOCKER_ENV_SH_LOADED:-}" ]; then
  return 0
fi
__ISOLATED_DOCKER_ENV_SH_LOADED=1

_hps_canonical_path() {
  realpath -m -- "$1"
}

_hps_path_is_within() {
  local path parent
  path="$(_hps_canonical_path "$1")" || return 2
  parent="$(_hps_canonical_path "$2")" || return 2
  case "$path" in
    "$parent"|"$parent"/*) return 0 ;;
    *) return 1 ;;
  esac
}

_hps_safe_temp_root() {
  local requested_root="${TMPDIR:-/tmp}"
  local project_root candidate_root

  project_root="$(_hps_canonical_path "$PROJECT_ROOT")" || return 1
  candidate_root="$(_hps_canonical_path "$requested_root")" || candidate_root=""
  if [ -z "$candidate_root" ] || _hps_path_is_within "$candidate_root" "$project_root"; then
    candidate_root="$(_hps_canonical_path /tmp)" || return 1
  fi
  if _hps_path_is_within "$candidate_root" "$project_root"; then
    printf '错误: 无法在工作区外创建隔离临时目录\n' >&2
    return 1
  fi
  printf '%s\n' "$candidate_root"
}

# 启动独立隔离环境：唯一项目名（hps_<prefix>_<run_id>）、工作区外 0700 临时
# 目录下的 0600 env、HPS_HTTP_PORT=0；依次调用一次 deploy、一次 base-url。
# 任一准备步骤、deploy 或 base-url 失败时，清理后保留原始失败码。
hps_start_isolated_environment() {
  local prefix="${1:-}"
  local run_id="${2:-}"
  local previous_umask temp_root temp_dir canonical_temp_dir rc=0
  local admin_username admin_email admin_password auth_secret mysql_root_password mysql_password

  HPS_ISOLATED_PROJECT_NAME="hps_${prefix}_${run_id}"
  HPS_ISOLATED_ENV_FILE=""
  HPS_ISOLATED_BASE_URL=""
  HPS_ISOLATED_TEMP_DIR=""
  HPS_ISOLATED_CLEANUP_DOWN_ATTEMPTED=0
  HPS_ISOLATED_CLEANUP_DOWN_RC=0
  HPS_ISOLATED_CLEANUP_ENV_REMOVED=0
  HPS_ISOLATED_CLEANUP_ENV_RM_RC=0
  HPS_ISOLATED_CLEANUP_DIR_REMOVED=0
  HPS_ISOLATED_CLEANUP_DIR_RM_RC=0

  previous_umask="$(umask)"
  umask 077

  if temp_root="$(_hps_safe_temp_root)"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
  if temp_dir="$(mktemp -d "$temp_root/hps_${prefix}_${run_id}.XXXXXX")"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
  if canonical_temp_dir="$(_hps_canonical_path "$temp_dir")"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
  if _hps_path_is_within "$canonical_temp_dir" "$PROJECT_ROOT"; then
    umask "$previous_umask"
    HPS_ISOLATED_TEMP_DIR="$canonical_temp_dir"
    hps_cleanup_isolated_environment || true
    return 1
  fi
  HPS_ISOLATED_TEMP_DIR="$canonical_temp_dir"

  if HPS_ISOLATED_ENV_FILE="$(mktemp "$HPS_ISOLATED_TEMP_DIR/environment.XXXXXX.env")"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi

  if auth_secret="$(openssl rand -hex 48)"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
  if mysql_root_password="$(openssl rand -hex 32)"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
  if mysql_password="$(openssl rand -hex 32)"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
  if admin_password="$(openssl rand -hex 32)"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi

  admin_username="admin_${run_id}"
  admin_email="${admin_username}@example.invalid"
  admin_password="Isolated_${prefix}_${admin_password}"

  if {
    printf 'AUTH_SECRET=%s\n' "$auth_secret"
    printf 'MYSQL_ROOT_PASSWORD=%s\n' "$mysql_root_password"
    printf 'MYSQL_USER=hps\n'
    printf 'MYSQL_PASSWORD=%s\n' "$mysql_password"
    printf 'HPS_HTTP_PORT=0\n'
    printf 'ADMIN_USERNAME=%s\n' "$admin_username"
    printf 'ADMIN_PASSWORD=%s\n' "$admin_password"
    printf 'ADMIN_EMAIL=%s\n' "$admin_email"
  } > "$HPS_ISOLATED_ENV_FILE"; then
    :
  else
    rc=$?
    umask "$previous_umask"
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
  umask "$previous_umask"

  if bash "$PROJECT_ROOT/scripts/docker.sh" deploy --project-name "$HPS_ISOLATED_PROJECT_NAME" \
    --env-file "$HPS_ISOLATED_ENV_FILE"; then
    :
  else
    rc=$?
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi

  if HPS_ISOLATED_BASE_URL="$(bash "$PROJECT_ROOT/scripts/docker.sh" base-url \
    --project-name "$HPS_ISOLATED_PROJECT_NAME" --env-file "$HPS_ISOLATED_ENV_FILE")"; then
    :
  else
    rc=$?
    hps_cleanup_isolated_environment || true
    return "$rc"
  fi
}

# 清理独立隔离环境：调用一次 down --volumes，随后删除 env 文件。
# env 文件已丢失时，down 省略 --env-file，但仍传 --project-name/--volumes。
# down 失败优先于 env 删除和临时目录删除失败作为返回码。
hps_cleanup_isolated_environment() {
  local env_file="${HPS_ISOLATED_ENV_FILE:-}"
  local temp_dir="${HPS_ISOLATED_TEMP_DIR:-}"
  local down_rc="${HPS_ISOLATED_CLEANUP_DOWN_RC:-0}"
  local env_rm_rc="${HPS_ISOLATED_CLEANUP_ENV_RM_RC:-0}"
  local dir_rm_rc="${HPS_ISOLATED_CLEANUP_DIR_RM_RC:-0}"
  local cleanup_step_rc=0
  local -a down_args=(down --project-name "${HPS_ISOLATED_PROJECT_NAME:-}")

  if [ "${HPS_ISOLATED_CLEANUP_DOWN_ATTEMPTED:-0}" -ne 1 ]; then
    HPS_ISOLATED_CLEANUP_DOWN_ATTEMPTED=1
    if [ -n "$env_file" ] && [ -f "$env_file" ]; then
      down_args+=(--env-file "$env_file")
    fi
    down_args+=(--volumes)

    if bash "$PROJECT_ROOT/scripts/docker.sh" "${down_args[@]}"; then
      HPS_ISOLATED_CLEANUP_DOWN_RC=0
    else
      HPS_ISOLATED_CLEANUP_DOWN_RC=$?
    fi
  fi
  down_rc="${HPS_ISOLATED_CLEANUP_DOWN_RC:-0}"
  if [ -n "$env_file" ] && [ -e "$env_file" ]; then
    if rm -f "$env_file"; then
      HPS_ISOLATED_CLEANUP_ENV_REMOVED=1
      HPS_ISOLATED_CLEANUP_ENV_RM_RC=0
    else
      cleanup_step_rc=$?
      HPS_ISOLATED_CLEANUP_ENV_REMOVED=0
      HPS_ISOLATED_CLEANUP_ENV_RM_RC=$cleanup_step_rc
    fi
  else
    HPS_ISOLATED_CLEANUP_ENV_REMOVED=1
    HPS_ISOLATED_CLEANUP_ENV_RM_RC=0
  fi
  env_rm_rc="${HPS_ISOLATED_CLEANUP_ENV_RM_RC:-0}"

  if [ -n "$temp_dir" ] && [ -e "$temp_dir" ]; then
    if rmdir -- "$temp_dir"; then
      HPS_ISOLATED_CLEANUP_DIR_REMOVED=1
      HPS_ISOLATED_CLEANUP_DIR_RM_RC=0
    else
      cleanup_step_rc=$?
      HPS_ISOLATED_CLEANUP_DIR_REMOVED=0
      HPS_ISOLATED_CLEANUP_DIR_RM_RC=$cleanup_step_rc
    fi
  else
    HPS_ISOLATED_CLEANUP_DIR_REMOVED=1
    HPS_ISOLATED_CLEANUP_DIR_RM_RC=0
  fi
  dir_rm_rc="${HPS_ISOLATED_CLEANUP_DIR_RM_RC:-0}"

  if [ "$down_rc" -ne 0 ]; then
    printf '错误: E2E 清理失败: docker down 退出码 %s\n' "$down_rc" >&2
  fi
  if [ "$env_rm_rc" -ne 0 ]; then
    printf '错误: E2E 清理失败: 临时 env 删除退出码 %s\n' "$env_rm_rc" >&2
  fi
  if [ "$dir_rm_rc" -ne 0 ]; then
    printf '错误: E2E 清理失败: 临时目录删除退出码 %s\n' "$dir_rm_rc" >&2
  fi
  if [ "$down_rc" -ne 0 ]; then
    return "$down_rc"
  fi
  if [ "$env_rm_rc" -ne 0 ]; then
    return "$env_rm_rc"
  fi
  return "$dir_rm_rc"
}

hps_restart_isolated_environment() {
  [ -n "${HPS_ISOLATED_PROJECT_NAME:-}" ] || return 1
  local -a restart_args=(restart --project-name "$HPS_ISOLATED_PROJECT_NAME")
  if [ -n "${HPS_ISOLATED_ENV_FILE:-}" ] && [ -f "$HPS_ISOLATED_ENV_FILE" ]; then
    restart_args+=(--env-file "$HPS_ISOLATED_ENV_FILE")
  fi
  bash "$PROJECT_ROOT/scripts/docker.sh" "${restart_args[@]}"
}

# 输出独立隔离环境的运行时指纹；stdout 原样来自 docker.sh。
hps_runtime_fingerprint() {
  bash "$PROJECT_ROOT/scripts/docker.sh" runtime-fingerprint \
    --project-name "$HPS_ISOLATED_PROJECT_NAME" --env-file "$HPS_ISOLATED_ENV_FILE"
}
