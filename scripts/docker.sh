#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib/common.sh"
cd "$ROOT"

ENV_FILE="${HPS_DOCKER_ENV_FILE:-$ROOT/.env}"
DEPLOY_WAIT_TIMEOUT="${DEPLOY_WAIT_TIMEOUT:-180}"
COMPOSE_PROJECT_NAME=""
REMOVE_VOLUMES=0
ENV_FILE_EXPLICIT=0

usage() {
  cat <<EOF
用法: $(basename "$0") <子命令> [参数]

子命令:
  deploy [--project-name <名称>] [--env-file <路径>]
           编译、构建镜像、启动全部服务并验证公共入口
  base-url [--project-name <名称>] [--env-file <路径>]
           输出当前 project 的 nginx 80 动态映射 URL
  runtime-fingerprint [--project-name <名称>] [--env-file <路径>]
           只读输出当前 project 后端运行镜像与 Compose 配置的稳定指纹
  status   显示服务状态并检查公共入口
  health   检查 nginx 到后端的公共健康接口
  stop     停止服务并保留数据卷
  build    编译 Release 后端和前端
  image    编译并构建后端镜像
  logs [--since <时长或时间戳>]
           输出全部服务日志，或输出指定时间范围内的日志
  all      deploy 的兼容别名
  up/run   deploy 的兼容别名
  down [--project-name <名称>] [--env-file <路径>] [--volumes]
           停止服务；--volumes 同时删除当前 project 的具名卷
EOF
}

logs_usage() {
  cat <<EOF
用法: $(basename "$0") logs [--since <时长或时间戳>]

不带参数时输出全部历史日志。--since 的值由 Docker 解析，可使用 10m 等时长，
也可使用 Docker 支持的时间戳。
EOF
}

compose() {
  local args=(--project-directory "$ROOT")
  [ -z "$COMPOSE_PROJECT_NAME" ] || args+=(--project-name "$COMPOSE_PROJECT_NAME")
  [ ! -f "$ENV_FILE" ] || args+=(--env-file "$ENV_FILE")
  docker compose "${args[@]}" "$@"
}

# 状态、日志和停止操作不需要真实密钥；占位值保证 .env 丢失时仍可回收容器。
compose_control() {
  AUTH_SECRET=control-command-not-used \
    MYSQL_ROOT_PASSWORD=control-command-not-used \
    MYSQL_PASSWORD=control-command-not-used \
    compose "$@"
}

parse_deployment_options() {
  local allow_volumes="$1"
  shift
  COMPOSE_PROJECT_NAME=""
  ENV_FILE="${HPS_DOCKER_ENV_FILE:-$ROOT/.env}"
  REMOVE_VOLUMES=0
  ENV_FILE_EXPLICIT=0
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --project-name)
        [ "$#" -ge 2 ] || { red "错误: --project-name 缺少值"; return 2; }
        [[ "$2" =~ ^[a-z0-9][a-z0-9_-]{0,62}$ ]] || {
          red "错误: --project-name 只允许小写字母、数字、下划线和连字符"
          return 2
        }
        COMPOSE_PROJECT_NAME="$2"
        shift 2
        ;;
      --env-file)
        [ "$#" -ge 2 ] || { red "错误: --env-file 缺少值"; return 2; }
        [ -n "$2" ] && [[ "$2" != -* ]] || { red "错误: --env-file 路径无效"; return 2; }
        ENV_FILE="$2"
        ENV_FILE_EXPLICIT=1
        shift 2
        ;;
      --volumes)
        [ "$allow_volumes" -eq 1 ] || { red "错误: 当前子命令不接受 --volumes"; return 2; }
        REMOVE_VOLUMES=1
        shift
        ;;
      *)
        red "错误: 未知部署参数: $1"
        return 2
        ;;
    esac
  done
  if [ "$ENV_FILE_EXPLICIT" -eq 1 ] && [ ! -f "$ENV_FILE" ]; then
    red "错误: 显式 --env-file 不存在: $ENV_FILE"
    return 2
  fi
}

reject_unexpected_args() {
  local command="$1"
  shift
  if [ "$#" -ne 0 ]; then
    red "错误: $command 不接受额外参数: $1"
    return 2
  fi
}

require_docker() {
  require_cmd docker || return 1
  if ! docker info >/dev/null; then
    red "错误: Docker 服务不可用"
    return 1
  fi
  if ! docker compose version >/dev/null; then
    red "错误: Docker Compose 插件不可用"
    return 1
  fi
}

require_deploy_tools() {
  require_docker || return 1
  require_cmd curl || return 1
  require_cmd openssl || return 1
  require_cmd xmake || return 1
  require_cmd npm || return 1
  require_cmd nproc || return 1
  if ! docker image inspect nginx:latest >/dev/null 2>&1; then
    red "错误: 本机不存在 nginx:latest 镜像，请先下载该镜像"
    return 1
  fi
}
env_file_value() {
  local key="$1" env_file="${2:-$ENV_FILE}" line value="" found=0
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      "$key="*)
        value="${line#*=}"
        found=1
        ;;
    esac
  done < "$env_file"
  if [ "$found" -ne 1 ]; then
    return 1
  fi
  if [ "${#value}" -ge 2 ]; then
    if { [ "${value:0:1}" = '"' ] && [ "${value: -1}" = '"' ]; } ||
       { [ "${value:0:1}" = "'" ] && [ "${value: -1}" = "'" ]; }; then
      value="${value:1:${#value}-2}"
    fi
  fi
  printf '%s\n' "$value"
}

validate_env_file() {
  local env_file="${1:-$ENV_FILE}"
  local auth_secret mysql_root_password mysql_user mysql_password http_port
  local admin_username="" admin_password="" admin_email=""
  local has_admin_username=0 has_admin_password=0 has_admin_email=0
  if ! auth_secret="$(env_file_value AUTH_SECRET "$env_file")" || [ "${#auth_secret}" -lt 32 ] ||
     [[ "$auth_secret" == replace-* ]]; then
    red "错误: .env 中 AUTH_SECRET 必须是至少 32 字符的非占位密钥"
    return 1
  fi
  if ! mysql_root_password="$(env_file_value MYSQL_ROOT_PASSWORD "$env_file")" ||
     [ "${#mysql_root_password}" -lt 16 ] || [[ "$mysql_root_password" == replace-* ]]; then
    red "错误: .env 中 MYSQL_ROOT_PASSWORD 必须是至少 16 字符的非占位密码"
    return 1
  fi
  if ! mysql_user="$(env_file_value MYSQL_USER "$env_file")" || [ -z "$mysql_user" ] || [ "$mysql_user" = "root" ]; then
    red "错误: .env 中 MYSQL_USER 必须是非 root 用户"
    return 1
  fi
  if ! mysql_password="$(env_file_value MYSQL_PASSWORD "$env_file")" || [ "${#mysql_password}" -lt 16 ] ||
     [[ "$mysql_password" == replace-* ]]; then
    red "错误: .env 中 MYSQL_PASSWORD 必须是至少 16 字符的非占位密码"
    return 1
  fi
  if [ "$mysql_password" = "$mysql_root_password" ]; then
    red "错误: MySQL 应用密码不得与 root 密码相同"
    return 1
  fi
  if ! http_port="$(env_file_value HPS_HTTP_PORT "$env_file")" || [[ ! "$http_port" =~ ^[0-9]+$ ]] ||
     [ "$http_port" -gt 65535 ]; then
    red "错误: .env 中 HPS_HTTP_PORT 必须是 0 到 65535 的整数"
    return 1
  fi

  if admin_username="$(env_file_value ADMIN_USERNAME "$env_file")"; then has_admin_username=1; fi
  if admin_password="$(env_file_value ADMIN_PASSWORD "$env_file")"; then has_admin_password=1; fi
  if admin_email="$(env_file_value ADMIN_EMAIL "$env_file")"; then has_admin_email=1; fi
  if [ "$has_admin_username" -eq 0 ] && [ "$has_admin_password" -eq 0 ] && [ "$has_admin_email" -eq 0 ]; then
    return 0
  fi
  if [ "$has_admin_username" -eq 1 ] && [ "$has_admin_password" -eq 1 ] && [ "$has_admin_email" -eq 1 ] &&
     [ -z "$admin_username" ] && [ -z "$admin_password" ] && [ -z "$admin_email" ]; then
    return 0
  fi
  if [ "$has_admin_username" -ne 1 ] || [ "$has_admin_password" -ne 1 ] || [ "$has_admin_email" -ne 1 ] ||
     [ -z "$admin_username" ] || [ -z "$admin_password" ] || [ -z "$admin_email" ]; then
    red "错误: ADMIN_USERNAME、ADMIN_PASSWORD、ADMIN_EMAIL 必须全部有效配置或全部禁用"
    return 1
  fi
  if [ "${#admin_username}" -lt 2 ] || [ "${#admin_username}" -gt 64 ] || [[ "$admin_username" == replace-* ]]; then
    red "错误: ADMIN_USERNAME 必须是 2 到 64 字符的非占位用户名"
    return 1
  fi
  if [ "${#admin_password}" -lt 16 ] || [[ "$admin_password" == replace-* ]]; then
    red "错误: ADMIN_PASSWORD 必须是至少 16 字符的非占位密码"
    return 1
  fi
  if [ "${#admin_email}" -gt 128 ] || [[ "$admin_email" == replace-* ]] ||
     [[ ! "$admin_email" =~ ^[^[:space:]@]+@[^[:space:]@]+\.[^[:space:]@]+$ ]]; then
    red "错误: ADMIN_EMAIL 必须是合法的非占位邮箱"
    return 1
  fi
}


ensure_env_file() {
  local existing_snapshot=""
  if [ -L "$ENV_FILE" ]; then
    red "错误: $ENV_FILE 不允许是符号链接"
    return 1
  fi
  if [ -f "$ENV_FILE" ]; then
    existing_snapshot="$(mktemp "${ENV_FILE}.existing.XXXXXX")" || {
      red "错误: 无法创建部署配置校验快照"
      return 1
    }
    rm -f "$existing_snapshot"
    if ! ln -P "$ENV_FILE" "$existing_snapshot" 2>/dev/null || [ -L "$existing_snapshot" ] ||
       [ ! -f "$existing_snapshot" ]; then
      rm -f "$existing_snapshot"
      red "错误: 部署配置在校验期间发生变化"
      return 1
    fi
    chmod 600 "$existing_snapshot"
    if ! validate_env_file "$existing_snapshot"; then
      rm -f "$existing_snapshot"
      return 1
    fi
    if [ -L "$ENV_FILE" ] || [ ! "$ENV_FILE" -ef "$existing_snapshot" ]; then
      rm -f "$existing_snapshot"
      red "错误: 部署配置在校验期间发生变化"
      return 1
    fi
    rm -f "$existing_snapshot"
    return 0
  fi

  blue "=== 生成部署密钥 ==="
  local auth_secret mysql_root_password mysql_password admin_suffix admin_username admin_password admin_email temp_file
  auth_secret="$(openssl rand -hex 48)"
  mysql_root_password="$(openssl rand -hex 32)"
  mysql_password="$(openssl rand -hex 32)"
  admin_suffix="$(openssl rand -hex 8)"
  admin_username="admin_${admin_suffix}"
  admin_password="$(openssl rand -hex 32)"
  admin_email="${admin_username}@localhost.invalid"

  temp_file="$(mktemp "${ENV_FILE}.tmp.XXXXXX")" || {
    red "错误: 无法创建临时部署配置"
    return 1
  }
  chmod 600 "$temp_file"
  if ! {
    printf 'AUTH_SECRET=%s\n' "$auth_secret"
    printf 'MYSQL_ROOT_PASSWORD=%s\n' "$mysql_root_password"
    printf 'MYSQL_USER=hps\n'
    printf 'MYSQL_PASSWORD=%s\n' "$mysql_password"
    printf 'HPS_HTTP_PORT=18080\n'
    printf 'ADMIN_USERNAME=%s\n' "$admin_username"
    printf 'ADMIN_PASSWORD=%s\n' "$admin_password"
    printf 'ADMIN_EMAIL=%s\n' "$admin_email"
  } > "$temp_file"; then
    rm -f "$temp_file"
    red "错误: 无法写入临时部署配置"
    return 1
  fi
  if ! validate_env_file "$temp_file"; then
    rm -f "$temp_file"
    return 1
  fi
  if ! ln "$temp_file" "$ENV_FILE" 2>/dev/null; then
    rm -f "$temp_file"
    red "错误: 部署配置目标已出现，拒绝覆盖"
    return 1
  fi
  rm -f "$temp_file"
  green "部署密钥已写入 $ENV_FILE（权限 0600）"
}

current_project_nginx_uses_port() {
  local port="$1" container_id state bindings binding
  if ! container_id="$(compose_control ps --quiet nginx 2>/dev/null)" || [ -z "$container_id" ]; then
    return 1
  fi
  if ! state="$(docker inspect --format '{{.State.Running}}' "$container_id" 2>/dev/null)" ||
     [ "$state" != "true" ]; then
    return 1
  fi
  if ! bindings="$(docker port "$container_id" 80/tcp 2>/dev/null)"; then
    return 1
  fi
  while IFS= read -r binding; do
    if [[ "$binding" == *":$port" ]]; then
      return 0
    fi
  done <<< "$bindings"
  return 1
}

host_port_is_listening() {
  local port="$1" port_hex file line_index local_address remote_address state ignored
  printf -v port_hex '%04X' "$port"
  for file in /proc/net/tcp /proc/net/tcp6; do
    [ -r "$file" ] || continue
    while read -r line_index local_address remote_address state ignored; do
      [ "$line_index" = "sl" ] && continue
      if [ "$state" = "0A" ] && [[ "$local_address" == *":$port_hex" ]]; then
        return 0
      fi
    done < "$file"
  done
  return 1
}

validate_http_port_availability() {
  local port container_ids container_id bindings binding container_name
  if ! port="$(env_file_value HPS_HTTP_PORT)"; then
    red "错误: 无法从 .env 读取 HPS_HTTP_PORT"
    return 1
  fi

  if [ "$port" = "0" ]; then
    green "端口预检通过: 由 Docker 动态分配宿主端口"
    return 0
  fi

  if current_project_nginx_uses_port "$port"; then
    green "端口预检通过: 当前项目 nginx 正在使用 127.0.0.1:$port，将原地更新"
    return 0
  fi

  if [ "$port" = "8080" ]; then
    red "错误: HPS_HTTP_PORT=8080 为 CodeQL 保留端口，请手工将 .env 设置为 HPS_HTTP_PORT=18080 后重试"
    return 1
  fi

  if ! container_ids="$(docker ps --quiet)"; then
    red "错误: 无法枚举 Docker 容器以检查端口 $port"
    return 1
  fi
  while IFS= read -r container_id; do
    [ -n "$container_id" ] || continue
    bindings="$(docker port "$container_id" 2>/dev/null || true)"
    while IFS= read -r binding; do
      if [[ "$binding" == *":$port" ]]; then
        container_name="$(docker inspect --format '{{.Name}}' "$container_id" 2>/dev/null || printf '%s' "$container_id")"
        red "错误: 端口 $port 已被 Docker 容器 ${container_name#/} 使用；8080 为 CodeQL 保留端口，建议使用 18080"
        return 1
      fi
    done <<< "$bindings"
  done <<< "$container_ids"

  if host_port_is_listening "$port"; then
    red "错误: 宿主机已有进程监听端口 $port；8080 为 CodeQL 保留端口，建议使用 18080"
    return 1
  fi

  green "端口预检通过: http://127.0.0.1:$port"
}

cmd_build() {
  blue "=== 编译 Release 后端 ==="
  xmake f -m release -y
  xmake -j"$(nproc)"

  blue "=== 构建前端 ==="
  ensure_frontend_dependencies
  (
    cd "$FRONTEND_DIR"
    npm run build
  )
  green "后端与前端构建成功"
}

build_image() {
  blue "=== 构建后端 Docker 镜像 ==="
  compose build high-performance-server
  green "后端镜像构建成功"
}

cmd_image() {
  require_deploy_tools
  ensure_env_file
  cmd_build
  compose config --quiet
  build_image
}

deployment_diagnostics() {
  red "=== 部署诊断：服务状态 ==="
  compose_control ps --all || true
  red "=== 部署诊断：完整服务日志 ==="
  compose_control logs --no-color || true
}

verify_service_health() {
  local service container_id state
  local services=(mysql high-performance-server nginx)
  for service in "${services[@]}"; do
    if ! container_id="$(compose_control ps --all --quiet "$service")" || [ -z "$container_id" ]; then
      red "服务未创建: $service"
      return 1
    fi
    if ! state="$(docker inspect --format '{{.State.Status}} {{if .State.Health}}{{.State.Health.Status}}{{else}}missing{{end}}' "$container_id")"; then
      red "无法读取服务状态: $service"
      return 1
    fi
    if [ "$state" != "running healthy" ]; then
      red "服务状态异常: $service ($state)"
      return 1
    fi
  done
}

print_backend_runtime_state() {
  local container_id runtime_state
  if ! container_id="$(compose_control ps --all --quiet high-performance-server)" || [ -z "$container_id" ]; then
    red "无法定位后端容器以读取运行时状态"
    return 1
  fi
  if ! runtime_state="$(docker inspect --format 'RestartCount={{.RestartCount}} OOMKilled={{.State.OOMKilled}}' "$container_id")"; then
    red "无法读取后端容器运行时状态"
    return 1
  fi
  printf '后端容器运行时状态: %s\n' "$runtime_state"
}

public_base_url() {
  local published line address port port_number family url
  local mapping_count=0 loopback_count=0 selected_url="" loopback_url=""
  if ! published="$(compose_control port nginx 80)"; then
    red "错误: 无法获取 nginx 公共端口"
    return 1
  fi
  if [ -z "$published" ]; then
    red "错误: nginx 公共端口映射为空"
    return 1
  fi

  while IFS= read -r line; do
    line="${line%$'\r'}"
    [ -n "$line" ] || { red "错误: nginx 公共端口包含空映射"; return 1; }
    port="${line##*:}"
    address="${line%:*}"
    if [ "$address" = "$line" ] || [[ ! "$port" =~ ^[0-9]+$ ]]; then
      red "错误: nginx 公共端口映射无效: $line"
      return 1
    fi
    port_number=$((10#$port))
    if [ "$port_number" -lt 1 ] || [ "$port_number" -gt 65535 ]; then
      red "错误: nginx 公共端口超出范围: $line"
      return 1
    fi

    if [[ "$address" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
      local octet
      local octets
      IFS='.' read -r -a octets <<< "$address"
      for octet in "${octets[@]}"; do
        [ "$((10#$octet))" -le 255 ] || { red "错误: nginx IPv4 映射无效: $line"; return 1; }
      done
      family=ipv4
      if [ "$address" = "0.0.0.0" ]; then
        url="http://127.0.0.1:$port_number"
      else
        url="http://$address:$port_number"
      fi
    else
      if [[ "$address" == \[*\] ]]; then
        address="${address:1:${#address}-2}"
      fi
      if [ "$address" != "::" ] && [ "$address" != "::1" ]; then
        red "错误: nginx IPv6 映射无效: $line"
        return 1
      fi
      family=ipv6
      url="http://[::1]:$port_number"
    fi

    mapping_count=$((mapping_count + 1))
    selected_url="$url"
    if [ "$family" = ipv4 ] && [ "$address" = "127.0.0.1" ]; then
      loopback_count=$((loopback_count + 1))
      loopback_url="$url"
    fi
  done <<< "$published"

  if [ "$mapping_count" -eq 1 ]; then
    printf '%s\n' "$selected_url"
    return 0
  fi
  if [ "$loopback_count" -eq 1 ]; then
    printf '%s\n' "$loopback_url"
    return 0
  fi
  red "错误: nginx 公共端口映射不唯一，且无法唯一选择 127.0.0.1 绑定"
  return 1
}

public_health_url() {
  local base_url
  base_url="$(public_base_url)" || return 1
  printf '%s/api/health\n' "$base_url"
}

cmd_base_url() {
  require_docker || return 1
  public_base_url
}

require_runtime_fingerprint_tools() {
  if ! command -v docker >/dev/null 2>&1; then
    red "错误: Docker 未安装" >&2
    return 1
  fi
  if ! docker info >/dev/null 2>&1; then
    red "错误: Docker 服务不可用" >&2
    return 1
  fi
  if ! docker compose version >/dev/null 2>&1; then
    red "错误: Docker Compose 插件不可用" >&2
    return 1
  fi
  if ! command -v sha256sum >/dev/null 2>&1; then
    red "错误: sha256sum 未安装" >&2
    return 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    red "错误: python3 未安装" >&2
    return 1
  fi
}

configured_service_image() {
  compose_control config --no-interpolate --format json high-performance-server 2>/dev/null |
    python3 -c '
import json
import sys

try:
    image = json.load(sys.stdin)["services"]["high-performance-server"]["image"]
except (json.JSONDecodeError, KeyError, TypeError):
    raise SystemExit(1)

if not isinstance(image, str) or not image or any(character.isspace() for character in image):
    raise SystemExit(1)

print(image)
'
}

cmd_runtime_fingerprint() {
  local compose_config_hash configured_image container_ids container_id image_id
  local runtime_images fingerprint
  local -a fingerprint_inputs

  require_runtime_fingerprint_tools || return 1
  if ! compose_control config --quiet >/dev/null 2>&1; then
    red "错误: Compose 配置无效或无法验证" >&2
    return 1
  fi
  if ! compose_config_hash="$(compose_control config --no-interpolate --hash high-performance-server 2>/dev/null |
    awk '$1 == "high-performance-server" { print $2; exit }')" ||
     [[ ! "$compose_config_hash" =~ ^[0-9a-fA-F]{64}$ ]]; then
    red "错误: 无法读取 high-performance-server Compose 配置" >&2
    return 1
  fi
  if ! configured_image="$(configured_service_image 2>/dev/null)" ||
     [[ ! "$configured_image" =~ ^[^[:space:]]+$ ]]; then
    red "错误: 无法解析 high-performance-server 镜像" >&2
    return 1
  fi
  if ! container_ids="$(compose_control ps --all --quiet high-performance-server 2>/dev/null)"; then
    red "错误: 无法查询 high-performance-server 容器" >&2
    return 1
  fi
  if ! runtime_images="$({
    while IFS= read -r container_id; do
      [ -n "$container_id" ] || continue
      if ! image_id="$(docker inspect --format '{{if .State.Running}}{{.Image}}{{end}}' "$container_id" 2>/dev/null)"; then
        exit 1
      fi
      [ -z "$image_id" ] || printf '%s\n' "$image_id"
    done <<< "$container_ids"
  })"; then
    red "错误: 无法读取 high-performance-server 运行镜像" >&2
    return 1
  fi

  fingerprint_inputs=(
    "compose-config-sha256:${compose_config_hash,,}"
    "configured-image:$configured_image"
  )
  if [ -n "$runtime_images" ]; then
    while IFS= read -r image_id; do
      [ -n "$image_id" ] && fingerprint_inputs+=("runtime-image:$image_id")
    done <<< "$runtime_images"
  fi
  if ! fingerprint="$(printf '%s\n' "${fingerprint_inputs[@]}" | LC_ALL=C sort -u | sha256sum | awk '{ print $1 }')" ||
     [[ ! "$fingerprint" =~ ^[0-9a-fA-F]{64}$ ]]; then
    red "错误: 无法生成运行时指纹" >&2
    return 1
  fi
  printf 'sha256:%s\n' "${fingerprint,,}"
}

cmd_health() {
  require_docker || return 1
  local url response
  if ! url="$(public_health_url)"; then
    return 1
  fi
  if ! response="$(curl -fsS --retry 5 --retry-delay 1 --retry-connrefused --max-time 10 "$url")"; then
    red "公共健康检查失败: $url"
    return 1
  fi
  case "$response" in
    *'"status":"ok"'*)
      green "公共健康检查通过: $url"
      printf '%s\n' "$response"
      ;;
    *)
      red "公共健康响应不符合预期: $response"
      return 1
      ;;
  esac
}

cmd_deploy() {
  require_deploy_tools
  ensure_env_file
  validate_env_file
  validate_http_port_availability
  cmd_build

  blue "=== 校验 Compose 配置 ==="
  compose config --quiet

  blue "=== 启动并等待全部服务健康 ==="
  if ! compose up --detach --build --wait --wait-timeout "$DEPLOY_WAIT_TIMEOUT" --remove-orphans; then
    deployment_diagnostics
    return 1
  fi
  if ! verify_service_health; then
    deployment_diagnostics
    return 1
  fi
  if ! cmd_health; then
    deployment_diagnostics
    return 1
  fi
  local health_url root_url
  if ! health_url="$(public_health_url)"; then
    deployment_diagnostics
    return 1
  fi
  root_url="${health_url%/api/health}"
  green "部署成功，服务入口: $root_url"
}

cmd_restart() {
  require_deploy_tools
  ensure_env_file
  validate_env_file
  blue "=== 重启后端服务并等待健康 ==="
  compose_control restart high-performance-server
  compose up --detach --wait --wait-timeout "$DEPLOY_WAIT_TIMEOUT" high-performance-server
  verify_service_health
  cmd_health
}

cmd_status() {
  require_docker || return 1
  compose_control ps --all
  local failed=0
  if ! print_backend_runtime_state; then
    failed=1
  fi
  if ! verify_service_health; then
    failed=1
  fi
  if ! cmd_health; then
    failed=1
  fi
  return "$failed"
}

cmd_stop() {
  require_docker || return 1
  blue "=== 停止服务（保留数据卷） ==="
  local down_args=(down --remove-orphans --timeout 30)
  [ "$REMOVE_VOLUMES" -eq 0 ] || down_args+=(--volumes)
  compose_control "${down_args[@]}"
  local remaining
  if ! remaining="$(compose_control ps --all --quiet)"; then
    red "错误: 无法验证服务是否已停止"
    return 1
  fi
  if [ -n "$remaining" ]; then
    red "错误: 停止后仍存在项目容器"
    compose_control ps --all
    return 1
  fi
  if [ "$REMOVE_VOLUMES" -eq 1 ]; then
    green "服务与当前 project 的具名卷已删除"
  else
    green "服务已停止，MySQL 与文件数据卷均已保留"
  fi
}

cmd_logs() {
  local since=""

  case "$#" in
    0) ;;
    1)
      if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
        logs_usage
        return 0
      fi
      red "错误: logs 只接受 --since <时长或时间戳>"
      logs_usage
      return 1
      ;;
    2)
      if [ "$1" != "--since" ]; then
        red "错误: logs 未知参数: $1"
        logs_usage
        return 1
      fi
      if [ -z "$2" ] || [[ "$2" == -* ]]; then
        red "错误: --since 需要有效的 Docker 时长或时间戳"
        logs_usage
        return 1
      fi
      since="$2"
      ;;
    *)
      red "错误: logs 参数过多"
      logs_usage
      return 1
      ;;
  esac

  require_docker || return 1
  if [ -n "$since" ]; then
    compose_control logs --no-color --since "$since"
  else
    compose_control logs --no-color
  fi
}

handle_menu_choice() {
  case "$1" in
    1) cmd_deploy ;;
    2) cmd_status ;;
    3) cmd_health ;;
    4) cmd_stop ;;
    5) cmd_build ;;
    6) cmd_logs ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "deploy:一键部署"
    "status:查看状态"
    "health:健康检查"
    "stop:停止服务"
    "build:构建后端与前端"
    "logs:查看完整日志"
  )
  menu_loop "Docker 部署工具（$ROOT）" "${items[@]}"
}

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
elif [ $# -gt 0 ]; then
  command="$1"
  shift
  case "$command" in
    deploy|all|up|run)
      parse_deployment_options 0 "$@"
      cmd_deploy
      ;;
    restart)
      parse_deployment_options 0 "$@"
      cmd_restart
      ;;
    base-url)
      parse_deployment_options 0 "$@"
      cmd_base_url
      ;;
    runtime-fingerprint)
      parse_deployment_options 0 "$@" >&2
      cmd_runtime_fingerprint
      ;;
    status)
      reject_unexpected_args "$command" "$@"
      cmd_status
      ;;
    health)
      reject_unexpected_args "$command" "$@"
      cmd_health
      ;;
    stop|down)
      parse_deployment_options 1 "$@"
      cmd_stop
      ;;
    build)
      reject_unexpected_args "$command" "$@"
      cmd_build
      ;;
    image)
      reject_unexpected_args "$command" "$@"
      cmd_image
      ;;
    logs)
      cmd_logs "$@"
      ;;
    -h|--help)
      reject_unexpected_args "$command" "$@"
      usage
      ;;
    *) red "未知子命令: $command"; usage; exit 1 ;;
  esac
else
  menu
fi
